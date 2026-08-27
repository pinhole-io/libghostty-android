package com.example.harn.terminal

import java.nio.ByteBuffer

/**
 * JNI bindings for libghostty-vt.
 *
 * Snapshot layout is documented in `ghostty_jni.cpp`.
 */
internal object GhosttyVt {
    const val SNAPSHOT_VERSION = 1

    const val DIRTY_NONE = 0
    const val DIRTY_PARTIAL = 1
    const val DIRTY_FULL = 2

    const val FLAG_BOLD = 1
    const val FLAG_ITALIC = 1 shl 1
    const val FLAG_FAINT = 1 shl 2
    const val FLAG_UNDERLINE = 1 shl 3
    const val FLAG_STRIKETHROUGH = 1 shl 4
    const val FLAG_INVERSE = 1 shl 5
    const val FLAG_INVISIBLE = 1 shl 6
    const val FLAG_FG_DEFAULT = 1 shl 7
    const val FLAG_BG_NONE = 1 shl 8

    const val CURSOR_BAR = 0
    const val CURSOR_BLOCK = 1
    const val CURSOR_UNDERLINE = 2
    const val CURSOR_BLOCK_HOLLOW = 3

    const val KEY_RELEASE = 0
    const val KEY_PRESS = 1
    const val KEY_REPEAT = 2

    init {
        System.loadLibrary("harn_ghostty")
    }

    @JvmStatic
    external fun nativeCreate(cols: Int, rows: Int, maxScrollback: Long): Long

    @JvmStatic
    external fun nativeFree(handle: Long)

    @JvmStatic
    external fun nativeWrite(handle: Long, data: ByteArray): ByteArray?

    @JvmStatic
    external fun nativeResize(
        handle: Long,
        cols: Int,
        rows: Int,
        cellWidthPx: Int,
        cellHeightPx: Int,
    )

    @JvmStatic
    external fun nativeScroll(handle: Long, deltaRows: Int)

    @JvmStatic
    external fun nativeScrollToBottom(handle: Long)

    @JvmStatic
    external fun nativeSnapshot(handle: Long, buffer: ByteBuffer): Int

    @JvmStatic
    external fun nativeEncodeKey(
        handle: Long,
        keyCode: Int,
        action: Int,
        metaState: Int,
        unshiftedCodepoint: Int,
        utf8: ByteArray?,
    ): ByteArray?
}
