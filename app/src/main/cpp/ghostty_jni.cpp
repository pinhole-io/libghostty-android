// JNI bridge from com.example.harn.terminal.GhosttyVt to libghostty-vt.
//
// One native session owns a terminal, render state, and key encoder.
// Snapshot writes dirty viewport rows into a direct ByteBuffer so Kotlin
// can paint without a JNI call per cell.
//
// Buffer layout (little-endian):
// header: 12 x i32
//   [0] version (1)
//   [1] dirty (0 none / 1 partial / 2 full)
//   [2] cols [3] rows
//   [4] default bg ARGB [5] default fg ARGB
//   [6] cursor x or -1 [7] cursor y or -1
//   [8] cursor style [9] cursor visible (0/1)
//   [10] cursor blinking (0/1) [11] row record count
// row record:
//   i32 rowIndex
//   cells[cols] x 16 bytes:
//     i32 fg ARGB, i32 bg ARGB, u16 textOffset, u16 textLen, u16 flags, u16 pad
//   u16 text[textUnits], zero-padded to a 4-byte boundary

#include <jni.h>

#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>

#include <cstdint>
#include <mutex>
#include <vector>

#include <ghostty/vt.h>

namespace {

constexpr const char* kLogTag = "harn-ghostty";

constexpr uint16_t kFlagBold = 1 << 0;
constexpr uint16_t kFlagItalic = 1 << 1;
constexpr uint16_t kFlagFaint = 1 << 2;
constexpr uint16_t kFlagUnderline = 1 << 3;
constexpr uint16_t kFlagStrikethrough = 1 << 4;
constexpr uint16_t kFlagInverse = 1 << 5;
constexpr uint16_t kFlagInvisible = 1 << 6;
constexpr uint16_t kFlagFgDefault = 1 << 7;
constexpr uint16_t kFlagBgNone = 1 << 8;

constexpr size_t kHeaderBytes = 12 * sizeof(int32_t);
constexpr size_t kRowIndexBytes = sizeof(int32_t);
constexpr size_t kCellRecordBytes = 16;
constexpr uint32_t kMaxCellTextUnits = 32;
constexpr int32_t kSnapshotVersion = 1;

struct Session {
    GhosttyTerminal term = nullptr;
    GhosttyRenderState renderState = nullptr;
    GhosttyRenderStateRowIterator rowIter = nullptr;
    GhosttyRenderStateRowCells cells = nullptr;
    GhosttyKeyEncoder encoder = nullptr;
    GhosttyKeyEvent keyEvent = nullptr;
    std::mutex mutex;
    std::vector<uint8_t> ptyOut;
};

void writePtyCallback(GhosttyTerminal, void* userdata, const uint8_t* data, size_t len) {
    auto* session = static_cast<Session*>(userdata);
    session->ptyOut.insert(session->ptyOut.end(), data, data + len);
}

Session* fromHandle(jlong handle) {
    return reinterpret_cast<Session*>(static_cast<intptr_t>(handle));
}

int32_t argb(GhosttyColorRgb color) {
    return static_cast<int32_t>(0xFF000000u | (static_cast<uint32_t>(color.r) << 16) |
                                (static_cast<uint32_t>(color.g) << 8) | color.b);
}

void destroySession(Session* session) {
    if (session == nullptr) {
        return;
    }
    ghostty_key_event_free(session->keyEvent);
    ghostty_key_encoder_free(session->encoder);
    ghostty_render_state_row_cells_free(session->cells);
    ghostty_render_state_row_iterator_free(session->rowIter);
    ghostty_render_state_free(session->renderState);
    ghostty_terminal_free(session->term);
    delete session;
}

GhosttyKey mapKey(int32_t keyCode) {
    if (keyCode >= AKEYCODE_A && keyCode <= AKEYCODE_Z) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_A + (keyCode - AKEYCODE_A));
    }
    if (keyCode >= AKEYCODE_0 && keyCode <= AKEYCODE_9) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_DIGIT_0 + (keyCode - AKEYCODE_0));
    }
    if (keyCode >= AKEYCODE_NUMPAD_0 && keyCode <= AKEYCODE_NUMPAD_9) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_NUMPAD_0 + (keyCode - AKEYCODE_NUMPAD_0));
    }
    if (keyCode >= AKEYCODE_F1 && keyCode <= AKEYCODE_F12) {
        return static_cast<GhosttyKey>(GHOSTTY_KEY_F1 + (keyCode - AKEYCODE_F1));
    }
    switch (keyCode) {
        case AKEYCODE_GRAVE:
            return GHOSTTY_KEY_BACKQUOTE;
        case AKEYCODE_BACKSLASH:
            return GHOSTTY_KEY_BACKSLASH;
        case AKEYCODE_LEFT_BRACKET:
            return GHOSTTY_KEY_BRACKET_LEFT;
        case AKEYCODE_RIGHT_BRACKET:
            return GHOSTTY_KEY_BRACKET_RIGHT;
        case AKEYCODE_COMMA:
            return GHOSTTY_KEY_COMMA;
        case AKEYCODE_EQUALS:
            return GHOSTTY_KEY_EQUAL;
        case AKEYCODE_MINUS:
            return GHOSTTY_KEY_MINUS;
        case AKEYCODE_PERIOD:
            return GHOSTTY_KEY_PERIOD;
        case AKEYCODE_APOSTROPHE:
            return GHOSTTY_KEY_QUOTE;
        case AKEYCODE_SEMICOLON:
            return GHOSTTY_KEY_SEMICOLON;
        case AKEYCODE_SLASH:
            return GHOSTTY_KEY_SLASH;
        case AKEYCODE_ALT_LEFT:
            return GHOSTTY_KEY_ALT_LEFT;
        case AKEYCODE_ALT_RIGHT:
            return GHOSTTY_KEY_ALT_RIGHT;
        case AKEYCODE_DEL:
            return GHOSTTY_KEY_BACKSPACE;
        case AKEYCODE_CAPS_LOCK:
            return GHOSTTY_KEY_CAPS_LOCK;
        case AKEYCODE_CTRL_LEFT:
            return GHOSTTY_KEY_CONTROL_LEFT;
        case AKEYCODE_CTRL_RIGHT:
            return GHOSTTY_KEY_CONTROL_RIGHT;
        case AKEYCODE_ENTER:
            return GHOSTTY_KEY_ENTER;
        case AKEYCODE_META_LEFT:
            return GHOSTTY_KEY_META_LEFT;
        case AKEYCODE_META_RIGHT:
            return GHOSTTY_KEY_META_RIGHT;
        case AKEYCODE_SHIFT_LEFT:
            return GHOSTTY_KEY_SHIFT_LEFT;
        case AKEYCODE_SHIFT_RIGHT:
            return GHOSTTY_KEY_SHIFT_RIGHT;
        case AKEYCODE_SPACE:
            return GHOSTTY_KEY_SPACE;
        case AKEYCODE_TAB:
            return GHOSTTY_KEY_TAB;
        case AKEYCODE_FORWARD_DEL:
            return GHOSTTY_KEY_DELETE;
        case AKEYCODE_MOVE_END:
            return GHOSTTY_KEY_END;
        case AKEYCODE_MOVE_HOME:
            return GHOSTTY_KEY_HOME;
        case AKEYCODE_INSERT:
            return GHOSTTY_KEY_INSERT;
        case AKEYCODE_PAGE_DOWN:
            return GHOSTTY_KEY_PAGE_DOWN;
        case AKEYCODE_PAGE_UP:
            return GHOSTTY_KEY_PAGE_UP;
        case AKEYCODE_DPAD_DOWN:
            return GHOSTTY_KEY_ARROW_DOWN;
        case AKEYCODE_DPAD_LEFT:
            return GHOSTTY_KEY_ARROW_LEFT;
        case AKEYCODE_DPAD_RIGHT:
            return GHOSTTY_KEY_ARROW_RIGHT;
        case AKEYCODE_DPAD_UP:
            return GHOSTTY_KEY_ARROW_UP;
        case AKEYCODE_NUM_LOCK:
            return GHOSTTY_KEY_NUM_LOCK;
        case AKEYCODE_NUMPAD_ADD:
            return GHOSTTY_KEY_NUMPAD_ADD;
        case AKEYCODE_NUMPAD_COMMA:
            return GHOSTTY_KEY_NUMPAD_COMMA;
        case AKEYCODE_NUMPAD_DOT:
            return GHOSTTY_KEY_NUMPAD_DECIMAL;
        case AKEYCODE_NUMPAD_DIVIDE:
            return GHOSTTY_KEY_NUMPAD_DIVIDE;
        case AKEYCODE_NUMPAD_ENTER:
            return GHOSTTY_KEY_NUMPAD_ENTER;
        case AKEYCODE_NUMPAD_EQUALS:
            return GHOSTTY_KEY_NUMPAD_EQUAL;
        case AKEYCODE_NUMPAD_MULTIPLY:
            return GHOSTTY_KEY_NUMPAD_MULTIPLY;
        case AKEYCODE_NUMPAD_SUBTRACT:
            return GHOSTTY_KEY_NUMPAD_SUBTRACT;
        case AKEYCODE_ESCAPE:
            return GHOSTTY_KEY_ESCAPE;
        default:
            return GHOSTTY_KEY_UNIDENTIFIED;
    }
}

GhosttyMods mapMods(int32_t metaState) {
    GhosttyMods mods = 0;
    if (metaState & AMETA_SHIFT_ON) {
        mods |= GHOSTTY_MODS_SHIFT;
    }
    if (metaState & AMETA_SHIFT_RIGHT_ON) {
        mods |= GHOSTTY_MODS_SHIFT_SIDE;
    }
    if (metaState & AMETA_CTRL_ON) {
        mods |= GHOSTTY_MODS_CTRL;
    }
    if (metaState & AMETA_CTRL_RIGHT_ON) {
        mods |= GHOSTTY_MODS_CTRL_SIDE;
    }
    if (metaState & AMETA_ALT_ON) {
        mods |= GHOSTTY_MODS_ALT;
    }
    if (metaState & AMETA_ALT_RIGHT_ON) {
        mods |= GHOSTTY_MODS_ALT_SIDE;
    }
    if (metaState & AMETA_META_ON) {
        mods |= GHOSTTY_MODS_SUPER;
    }
    if (metaState & AMETA_META_RIGHT_ON) {
        mods |= GHOSTTY_MODS_SUPER_SIDE;
    }
    if (metaState & AMETA_CAPS_LOCK_ON) {
        mods |= GHOSTTY_MODS_CAPS_LOCK;
    }
    if (metaState & AMETA_NUM_LOCK_ON) {
        mods |= GHOSTTY_MODS_NUM_LOCK;
    }
    return mods;
}

uint32_t appendUtf16(uint32_t cp, uint16_t* out, uint32_t used, uint32_t cap) {
    if (cp < 0x10000) {
        if (used + 1 > cap) {
            return 0;
        }
        out[used] = static_cast<uint16_t>(cp);
        return 1;
    }
    if (used + 2 > cap) {
        return 0;
    }
    cp -= 0x10000;
    out[used] = static_cast<uint16_t>(0xD800 + (cp >> 10));
    out[used + 1] = static_cast<uint16_t>(0xDC00 + (cp & 0x3FF));
    return 2;
}

jbyteArray copyPtyOut(JNIEnv* env, Session* session) {
    if (session->ptyOut.empty()) {
        return nullptr;
    }
    const auto len = static_cast<jsize>(session->ptyOut.size());
    jbyteArray out = env->NewByteArray(len);
    if (out == nullptr) {
        return nullptr;
    }
    env->SetByteArrayRegion(out, 0, len, reinterpret_cast<const jbyte*>(session->ptyOut.data()));
    return out;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_com_example_harn_terminal_GhosttyVt_nativeCreate(
        JNIEnv*, jclass, jint cols, jint rows, jlong maxScrollbackLines) {
    auto* session = new Session();
    if (ghostty_terminal_new(nullptr, &session->term, static_cast<uint16_t>(cols),
                             static_cast<uint16_t>(rows)) != GHOSTTY_SUCCESS ||
        ghostty_render_state_new(nullptr, &session->renderState) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_iterator_new(nullptr, &session->rowIter) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_cells_new(nullptr, &session->cells) != GHOSTTY_SUCCESS ||
        ghostty_key_encoder_new(nullptr, &session->encoder) != GHOSTTY_SUCCESS ||
        ghostty_key_event_new(nullptr, &session->keyEvent) != GHOSTTY_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "failed to create terminal session");
        destroySession(session);
        return 0;
    }

    const size_t scrollback = static_cast<size_t>(maxScrollbackLines);
    ghostty_terminal_set(session->term, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &scrollback);
    ghostty_terminal_set(session->term, GHOSTTY_TERMINAL_OPT_USERDATA, session);
    ghostty_terminal_set(session->term, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                         reinterpret_cast<const void*>(&writePtyCallback));

    GhosttyColorRgb bg{0x1A, 0x1A, 0x1A};
    GhosttyColorRgb fg{0xE6, 0xE6, 0xE6};
    ghostty_terminal_set(session->term, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);
    ghostty_terminal_set(session->term, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);

    const bool blinkDefault = true;
    ghostty_terminal_set(session->term, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_BLINK, &blinkDefault);

    return static_cast<jlong>(reinterpret_cast<intptr_t>(session));
}

JNIEXPORT void JNICALL Java_com_example_harn_terminal_GhosttyVt_nativeFree(JNIEnv*, jclass,
                                                                          jlong handle) {
    destroySession(fromHandle(handle));
}

JNIEXPORT jbyteArray JNICALL Java_com_example_harn_terminal_GhosttyVt_nativeWrite(
        JNIEnv* env, jclass, jlong handle, jbyteArray data) {
    auto* session = fromHandle(handle);
    if (session == nullptr || data == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    session->ptyOut.clear();
    const jsize len = env->GetArrayLength(data);
    if (len > 0) {
        jbyte* bytes = env->GetByteArrayElements(data, nullptr);
        ghostty_terminal_vt_write(session->term, reinterpret_cast<const uint8_t*>(bytes),
                                  static_cast<size_t>(len));
        env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    }
    return copyPtyOut(env, session);
}

JNIEXPORT void JNICALL Java_com_example_harn_terminal_GhosttyVt_nativeResize(
        JNIEnv*, jclass, jlong handle, jint cols, jint rows, jint cellWidthPx, jint cellHeightPx) {
    auto* session = fromHandle(handle);
    if (session == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    ghostty_terminal_resize(session->term, static_cast<uint16_t>(cols), static_cast<uint16_t>(rows),
                            static_cast<uint32_t>(cellWidthPx), static_cast<uint32_t>(cellHeightPx));
    GhosttyRenderStateDirty full = GHOSTTY_RENDER_STATE_DIRTY_FULL;
    ghostty_render_state_set(session->renderState, GHOSTTY_RENDER_STATE_OPTION_DIRTY, &full);
}

JNIEXPORT void JNICALL Java_com_example_harn_terminal_GhosttyVt_nativeScroll(
        JNIEnv*, jclass, jlong handle, jint deltaRows) {
    auto* session = fromHandle(handle);
    if (session == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    GhosttyTerminalScrollViewport behavior{};
    behavior.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
    behavior.value.delta = deltaRows;
    ghostty_terminal_scroll_viewport(session->term, behavior);
}

JNIEXPORT void JNICALL Java_com_example_harn_terminal_GhosttyVt_nativeScrollToBottom(
        JNIEnv*, jclass, jlong handle) {
    auto* session = fromHandle(handle);
    if (session == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(session->mutex);
    GhosttyTerminalScrollViewport behavior{};
    behavior.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
    ghostty_terminal_scroll_viewport(session->term, behavior);
}

JNIEXPORT jint JNICALL Java_com_example_harn_terminal_GhosttyVt_nativeSnapshot(
        JNIEnv* env, jclass, jlong handle, jobject buffer) {
    auto* session = fromHandle(handle);
    if (session == nullptr || buffer == nullptr) {
        return -1;
    }
    auto* base = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
    const auto cap = static_cast<size_t>(env->GetDirectBufferCapacity(buffer));
    if (base == nullptr || cap < kHeaderBytes) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(session->mutex);
    if (ghostty_render_state_update(session->renderState, session->term) != GHOSTTY_SUCCESS) {
        return -1;
    }

    GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_get(session->renderState, GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);

    uint16_t cols = 0;
    uint16_t rows = 0;
    ghostty_render_state_get(session->renderState, GHOSTTY_RENDER_STATE_DATA_COLS, &cols);
    ghostty_render_state_get(session->renderState, GHOSTTY_RENDER_STATE_DATA_ROWS, &rows);

    GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    ghostty_render_state_get(session->renderState, GHOSTTY_RENDER_STATE_DATA_COLORS, &colors);

    GhosttyRenderStateCursor cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    ghostty_render_state_get(session->renderState, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cursor);

    auto* header = reinterpret_cast<int32_t*>(base);
    header[0] = kSnapshotVersion;
    header[1] = static_cast<int32_t>(dirty);
    header[2] = cols;
    header[3] = rows;
    header[4] = argb(colors.background);
    header[5] = argb(colors.foreground);
    header[6] = cursor.viewport_has_value ? cursor.viewport_x : -1;
    header[7] = cursor.viewport_has_value ? cursor.viewport_y : -1;
    header[8] = static_cast<int32_t>(cursor.visual_style);
    header[9] = cursor.visible ? 1 : 0;
    header[10] = cursor.blinking ? 1 : 0;
    header[11] = 0;

    if (dirty == GHOSTTY_RENDER_STATE_DIRTY_FALSE) {
        return 0;
    }

    if (ghostty_render_state_get(session->renderState, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                 &session->rowIter) != GHOSTTY_SUCCESS) {
        return -1;
    }

    size_t offset = kHeaderBytes;
    int32_t rowCount = 0;
    uint16_t rowY = 0;
    std::vector<uint32_t> graphemeBuf;

    while (ghostty_render_state_row_iterator_next_dirty(session->rowIter, &rowY)) {
        const size_t cellsOffset = offset + kRowIndexBytes;
        const size_t textOffset = cellsOffset + static_cast<size_t>(cols) * kCellRecordBytes;
        if (textOffset > cap) {
            return -1;
        }

        *reinterpret_cast<int32_t*>(base + offset) = rowY;

        if (ghostty_render_state_row_get(session->rowIter, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                         &session->cells) != GHOSTTY_SUCCESS) {
            return -1;
        }

        auto* textBlob = reinterpret_cast<uint16_t*>(base + textOffset);
        const uint32_t textCapUnits = static_cast<uint32_t>((cap - textOffset) / sizeof(uint16_t));
        uint32_t textUnits = 0;
        int32_t col = 0;
        while (col < cols && ghostty_render_state_row_cells_next(session->cells)) {
            uint8_t* record = base + cellsOffset + static_cast<size_t>(col) * kCellRecordBytes;
            uint16_t flags = 0;
            int32_t fg = 0;
            int32_t bg = 0;

            bool hasStyling = false;
            ghostty_render_state_row_cells_get(session->cells,
                                               GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_HAS_STYLING,
                                               &hasStyling);
            if (hasStyling) {
                GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
                ghostty_render_state_row_cells_get(
                        session->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
                if (style.bold) flags |= kFlagBold;
                if (style.italic) flags |= kFlagItalic;
                if (style.faint) flags |= kFlagFaint;
                if (style.underline != 0) flags |= kFlagUnderline;
                if (style.strikethrough) flags |= kFlagStrikethrough;
                if (style.inverse) flags |= kFlagInverse;
                if (style.invisible) flags |= kFlagInvisible;
            }

            GhosttyColorRgb cellColor{};
            if (ghostty_render_state_row_cells_get(session->cells,
                                                   GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                                                   &cellColor) == GHOSTTY_SUCCESS) {
                fg = argb(cellColor);
            } else {
                flags |= kFlagFgDefault;
            }
            if (ghostty_render_state_row_cells_get(session->cells,
                                                   GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                                                   &cellColor) == GHOSTTY_SUCCESS) {
                bg = argb(cellColor);
            } else {
                flags |= kFlagBgNone;
            }

            uint32_t graphemeLen = 0;
            ghostty_render_state_row_cells_get(
                    session->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &graphemeLen);

            const uint32_t cellTextStart = textUnits;
            uint32_t cellUnits = 0;
            if (graphemeLen > 0) {
                graphemeBuf.resize(graphemeLen);
                ghostty_render_state_row_cells_get(
                        session->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                        graphemeBuf.data());
                for (uint32_t i = 0; i < graphemeLen && cellUnits < kMaxCellTextUnits; i++) {
                    const uint32_t written =
                            appendUtf16(graphemeBuf[i], textBlob, textUnits + cellUnits, textCapUnits);
                    if (written == 0) {
                        break;
                    }
                    cellUnits += written;
                }
            }
            textUnits += cellUnits;

            *reinterpret_cast<int32_t*>(record) = fg;
            *reinterpret_cast<int32_t*>(record + 4) = bg;
            *reinterpret_cast<uint16_t*>(record + 8) = static_cast<uint16_t>(cellTextStart);
            *reinterpret_cast<uint16_t*>(record + 10) = static_cast<uint16_t>(cellUnits);
            *reinterpret_cast<uint16_t*>(record + 12) = flags;
            *reinterpret_cast<uint16_t*>(record + 14) = 0;
            col++;
        }

        const size_t textBytes = ((static_cast<size_t>(textUnits) * sizeof(uint16_t)) + 3u) & ~3u;
        offset = textOffset + textBytes;
        if (offset > cap) {
            return -1;
        }
        rowCount++;
    }

    header[11] = rowCount;
    ghostty_render_state_clean(session->renderState);
    return rowCount;
}

JNIEXPORT jbyteArray JNICALL Java_com_example_harn_terminal_GhosttyVt_nativeEncodeKey(
        JNIEnv* env, jclass, jlong handle, jint keyCode, jint action, jint metaState,
        jint unshiftedCodepoint, jbyteArray utf8) {
    auto* session = fromHandle(handle);
    if (session == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(session->mutex);

    ghostty_key_encoder_setopt_from_terminal(session->encoder, session->term);

    GhosttyKeyAction keyAction = GHOSTTY_KEY_ACTION_PRESS;
    if (action == 0) {
        keyAction = GHOSTTY_KEY_ACTION_RELEASE;
    } else if (action == 2) {
        keyAction = GHOSTTY_KEY_ACTION_REPEAT;
    }

    ghostty_key_event_set_action(session->keyEvent, keyAction);
    ghostty_key_event_set_key(session->keyEvent, mapKey(keyCode));
    ghostty_key_event_set_mods(session->keyEvent, mapMods(metaState));
    ghostty_key_event_set_consumed_mods(session->keyEvent, 0);
    ghostty_key_event_set_composing(session->keyEvent, false);
    ghostty_key_event_set_unshifted_codepoint(session->keyEvent,
                                              static_cast<uint32_t>(unshiftedCodepoint));

    jbyte* utf8Bytes = nullptr;
    jsize utf8Len = 0;
    if (utf8 != nullptr) {
        utf8Len = env->GetArrayLength(utf8);
        if (utf8Len > 0) {
            utf8Bytes = env->GetByteArrayElements(utf8, nullptr);
        }
    }
    ghostty_key_event_set_utf8(session->keyEvent, reinterpret_cast<const char*>(utf8Bytes),
                               static_cast<size_t>(utf8Bytes != nullptr ? utf8Len : 0));

    char stackBuf[128];
    size_t written = 0;
    GhosttyResult result = ghostty_key_encoder_encode(session->encoder, session->keyEvent, stackBuf,
                                                      sizeof(stackBuf), &written);
    std::vector<char> heapBuf;
    const char* out = stackBuf;
    if (result == GHOSTTY_OUT_OF_SPACE) {
        heapBuf.resize(written);
        result = ghostty_key_encoder_encode(session->encoder, session->keyEvent, heapBuf.data(),
                                            heapBuf.size(), &written);
        out = heapBuf.data();
    }

    ghostty_key_event_set_utf8(session->keyEvent, nullptr, 0);
    if (utf8Bytes != nullptr) {
        env->ReleaseByteArrayElements(utf8, utf8Bytes, JNI_ABORT);
    }

    if (result != GHOSTTY_SUCCESS || written == 0) {
        return nullptr;
    }
    jbyteArray outArray = env->NewByteArray(static_cast<jsize>(written));
    if (outArray == nullptr) {
        return nullptr;
    }
    env->SetByteArrayRegion(outArray, 0, static_cast<jsize>(written),
                            reinterpret_cast<const jbyte*>(out));
    return outArray;
}

}  // extern "C"
