package com.example.harn.terminal

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Typeface
import android.text.InputType
import android.util.AttributeSet
import android.util.TypedValue
import android.view.Choreographer
import android.view.GestureDetector
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.BaseInputConnection
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.view.inputmethod.InputMethodManager
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.roundToInt

/**
 * Canvas renderer for a libghostty-vt session attached to a local PTY.
 */
class TerminalView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : View(context, attrs) {
    private var handle: Long = GhosttyVt.nativeCreate(INITIAL_COLS, INITIAL_ROWS, MAX_SCROLLBACK)
    private var pty: LocalPty? = null
    private var readerThread: Thread? = null

    private val textPaint = newTextPaint(Typeface.MONOSPACE)
    private val boldPaint = newTextPaint(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD))
    private val italicPaint = newTextPaint(Typeface.create(Typeface.MONOSPACE, Typeface.ITALIC))
    private val boldItalicPaint =
        newTextPaint(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD_ITALIC))
    private val fillPaint = Paint()
    private val cursorPaint = Paint()
    private val underlinePaint = Paint(Paint.ANTI_ALIAS_FLAG)

    private var cellWidth = textPaint.measureText("M")
    private var cellHeight = ceil(
        textPaint.fontMetrics.descent - textPaint.fontMetrics.ascent,
    ).toInt().coerceAtLeast(1)
    private var baseline = -textPaint.fontMetrics.ascent
    private val underlineThickness = max(1f, resources.displayMetrics.density)

    private var cols = INITIAL_COLS
    private var rows = INITIAL_ROWS
    private var bitmap: Bitmap? = null
    private var bitmapCanvas: Canvas? = null
    private var snapshotBuf: ByteBuffer? = null

    private var defaultBg = 0xFF1A1A1A.toInt()
    private var defaultFg = 0xFFE6E6E6.toInt()
    private var cursorX = -1
    private var cursorY = -1
    private var cursorStyle = GhosttyVt.CURSOR_BLOCK
    private var cursorVisible = false
    private var cursorBlinks = false
    private var blinkPhaseOn = true
    private var blinkScheduled = false

    private var framePending = false
    private val frameCallback = Choreographer.FrameCallback {
        framePending = false
        renderFrame()
    }
    private val blinkRunnable = object : Runnable {
        override fun run() {
            if (!blinkScheduled) {
                return
            }
            blinkPhaseOn = !blinkPhaseOn
            invalidate()
            postDelayed(this, BLINK_INTERVAL_MS)
        }
    }

    private val gestureDetector = GestureDetector(
        context,
        object : GestureDetector.SimpleOnGestureListener() {
            override fun onDown(e: MotionEvent): Boolean = true

            override fun onScroll(
                e1: MotionEvent?,
                e2: MotionEvent,
                distanceX: Float,
                distanceY: Float,
            ): Boolean {
                val deltaRows = (distanceY / cellHeight).roundToInt()
                if (deltaRows != 0 && handle != 0L) {
                    GhosttyVt.nativeScroll(handle, deltaRows)
                    requestFrame()
                }
                return true
            }

            override fun onSingleTapUp(e: MotionEvent): Boolean {
                requestFocus()
                showIme()
                return true
            }
        },
    )

    init {
        isFocusable = true
        isFocusableInTouchMode = true
        keepScreenOn = true
        setBackgroundColor(defaultBg)
        if (handle == 0L) {
            throw IllegalStateException("libghostty-vt failed to create a terminal")
        }
    }

    fun startSession() {
        if (pty != null) {
            return
        }
        val filesDir = context.filesDir.absolutePath
        val cacheDir = context.cacheDir.absolutePath
        val env = arrayOf(
            "TERM=xterm-256color",
            "COLORTERM=truecolor",
            "HOME=$filesDir",
            "TMPDIR=$cacheDir",
            "PATH=/system/bin:/system/xbin:/vendor/bin",
        )
        val cellW = cellWidth.roundToInt().coerceAtLeast(1)
        pty = LocalPty.start(
            cwd = filesDir,
            shell = "/system/bin/sh",
            env = env,
            cols = cols,
            rows = rows,
            cellWidthPx = cellW,
            cellHeightPx = cellHeight,
        )
        if (pty == null) {
            feedPtyOutput("failed to start local PTY\r\n".toByteArray(Charsets.UTF_8))
            return
        }
        readerThread = Thread({ readPtyLoop() }, "harn-pty-read").also { thread ->
            thread.isDaemon = true
            thread.start()
        }
        requestFocus()
        post { showIme() }
    }

    fun stopSession() {
        blinkScheduled = false
        removeCallbacks(blinkRunnable)
        readerThread?.interrupt()
        readerThread = null
        pty?.close()
        pty = null
        if (handle != 0L) {
            GhosttyVt.nativeFree(handle)
            handle = 0L
        }
        bitmap?.recycle()
        bitmap = null
        bitmapCanvas = null
    }

    override fun onDetachedFromWindow() {
        stopSession()
        super.onDetachedFromWindow()
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        applyGridSize(w, h)
    }

    override fun setPadding(left: Int, top: Int, right: Int, bottom: Int) {
        val changed =
            left != paddingLeft || top != paddingTop || right != paddingRight || bottom != paddingBottom
        super.setPadding(left, top, right, bottom)
        if (changed && width > 0 && height > 0) {
            applyGridSize(width, height)
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        return gestureDetector.onTouchEvent(event) || super.onTouchEvent(event)
    }

    override fun onCheckIsTextEditor(): Boolean = true

    override fun onCreateInputConnection(outAttrs: EditorInfo): InputConnection {
        outAttrs.inputType = InputType.TYPE_NULL
        outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI or
            EditorInfo.IME_FLAG_NO_FULLSCREEN
        return object : BaseInputConnection(this, false) {
            override fun commitText(text: CharSequence, newCursorPosition: Int): Boolean {
                sendToPty(text.toString().toByteArray(Charsets.UTF_8))
                return true
            }

            override fun deleteSurroundingText(beforeLength: Int, afterLength: Int): Boolean {
                if (beforeLength > 0) {
                    sendEncodedKey(KeyEvent.KEYCODE_DEL, GhosttyVt.KEY_PRESS, 0)
                }
                return true
            }

            override fun sendKeyEvent(event: KeyEvent): Boolean {
                return onKeyDown(event.keyCode, event) || super.sendKeyEvent(event)
            }
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val action = if (event.repeatCount > 0) GhosttyVt.KEY_REPEAT else GhosttyVt.KEY_PRESS
        return sendEncodedKey(keyCode, action, event.metaState, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        sendEncodedKey(keyCode, GhosttyVt.KEY_RELEASE, event.metaState, event)
        return true
    }

    override fun onDraw(canvas: Canvas) {
        val save = canvas.save()
        canvas.translate(paddingLeft.toFloat(), paddingTop.toFloat())
        val grid = bitmap
        if (grid != null) {
            canvas.drawBitmap(grid, 0f, 0f, null)
        } else {
            canvas.drawColor(defaultBg)
        }
        drawCursor(canvas)
        canvas.restoreToCount(save)
    }

    private fun applyGridSize(widthPx: Int, heightPx: Int) {
        if (handle == 0L) {
            return
        }
        val contentW = (widthPx - paddingLeft - paddingRight).coerceAtLeast(1)
        val contentH = (heightPx - paddingTop - paddingBottom).coerceAtLeast(1)
        val newCols = max(2, (contentW / cellWidth).toInt())
        val newRows = max(2, contentH / cellHeight)
        if (newCols == cols && newRows == rows && bitmap != null &&
            bitmap?.width == contentW && bitmap?.height == contentH
        ) {
            return
        }
        cols = newCols
        rows = newRows
        val cellW = cellWidth.roundToInt().coerceAtLeast(1)
        GhosttyVt.nativeResize(handle, cols, rows, cellW, cellHeight)
        pty?.setWindowSize(cols, rows, cellW, cellHeight)
        bitmap?.recycle()
        bitmap = Bitmap.createBitmap(contentW, contentH, Bitmap.Config.ARGB_8888)
        bitmapCanvas = Canvas(bitmap!!)
        bitmapCanvas!!.drawColor(defaultBg)
        snapshotBuf = ByteBuffer.allocateDirect(snapshotCapacity(cols, rows))
            .order(ByteOrder.LITTLE_ENDIAN)
        requestFrame()
    }

    private fun readPtyLoop() {
        val buffer = ByteArray(8192)
        while (!Thread.currentThread().isInterrupted) {
            val n = try {
                pty?.read(buffer) ?: -1
            } catch (_: Exception) {
                -1
            }
            if (n <= 0) {
                break
            }
            val chunk = buffer.copyOf(n)
            post { feedPtyOutput(chunk) }
        }
    }

    private fun feedPtyOutput(data: ByteArray) {
        if (handle == 0L || data.isEmpty()) {
            return
        }
        val reply = GhosttyVt.nativeWrite(handle, data)
        if (reply != null) {
            sendToPty(reply)
        }
        GhosttyVt.nativeScrollToBottom(handle)
        requestFrame()
    }

    private fun sendToPty(bytes: ByteArray) {
        if (bytes.isEmpty()) {
            return
        }
        try {
            pty?.write(bytes)
        } catch (_: Exception) {
            // The PTY closed. The reader thread exits on the next read.
        }
        GhosttyVt.nativeScrollToBottom(handle)
    }

    private fun sendEncodedKey(
        keyCode: Int,
        action: Int,
        metaState: Int,
        event: KeyEvent? = null,
    ): Boolean {
        if (handle == 0L) {
            return false
        }
        val utf8 = event?.unicodeChar
            ?.takeIf { it != 0 && it and KeyCharacterMap.COMBINING_ACCENT == 0 }
            ?.let { intArrayOf(it).toUtf8() }
        val encoded = GhosttyVt.nativeEncodeKey(
            handle,
            keyCode,
            action,
            metaState,
            event?.unicodeChar ?: 0,
            utf8,
        ) ?: return false
        sendToPty(encoded)
        return true
    }

    private fun requestFrame() {
        if (framePending) {
            return
        }
        framePending = true
        Choreographer.getInstance().postFrameCallback(frameCallback)
    }

    private fun renderFrame() {
        if (handle == 0L) {
            return
        }
        var buffer = snapshotBuf ?: return
        var rowCount = GhosttyVt.nativeSnapshot(handle, buffer)
        if (rowCount < 0) {
            buffer = ByteBuffer.allocateDirect(buffer.capacity() * 2)
                .order(ByteOrder.LITTLE_ENDIAN)
            snapshotBuf = buffer
            rowCount = GhosttyVt.nativeSnapshot(handle, buffer)
        }
        if (rowCount < 0) {
            return
        }
        buffer.order(ByteOrder.LITTLE_ENDIAN)
        buffer.position(0)
        val version = buffer.int
        if (version != GhosttyVt.SNAPSHOT_VERSION) {
            return
        }
        val dirty = buffer.int
        cols = buffer.int
        rows = buffer.int
        defaultBg = buffer.int
        defaultFg = buffer.int
        cursorX = buffer.int
        cursorY = buffer.int
        cursorStyle = buffer.int
        cursorVisible = buffer.int != 0
        cursorBlinks = buffer.int != 0
        val records = buffer.int
        if (dirty != GhosttyVt.DIRTY_NONE) {
            paintRows(buffer, records)
        }
        updateBlink()
        invalidate()
    }

    private fun paintRows(buffer: ByteBuffer, recordCount: Int) {
        val canvas = bitmapCanvas ?: return
        val chars = CharArray(32)
        repeat(recordCount) {
            val rowIndex = buffer.int
            val y = rowIndex * cellHeight.toFloat()
            val cellStart = buffer.position()
            var textUnits = 0
            for (col in 0 until cols) {
                buffer.int
                buffer.int
                val off = buffer.short.toInt() and 0xFFFF
                val len = buffer.short.toInt() and 0xFFFF
                buffer.int
                textUnits = max(textUnits, off + len)
            }
            val textStart = cellStart + cols * CELL_RECORD_BYTES
            buffer.position(cellStart)
            for (col in 0 until cols) {
                val fgRaw = buffer.int
                val bgRaw = buffer.int
                val textOffset = buffer.short.toInt() and 0xFFFF
                val textLen = buffer.short.toInt() and 0xFFFF
                val flags = buffer.short.toInt() and 0xFFFF
                buffer.short
                var fg = if (flags and GhosttyVt.FLAG_FG_DEFAULT != 0) defaultFg else fgRaw
                var bg = if (flags and GhosttyVt.FLAG_BG_NONE != 0) defaultBg else bgRaw
                if (flags and GhosttyVt.FLAG_INVERSE != 0) {
                    val tmp = fg
                    fg = bg
                    bg = tmp
                }
                if (flags and GhosttyVt.FLAG_INVISIBLE != 0) {
                    fg = bg
                }
                val x = col * cellWidth
                fillPaint.color = bg
                canvas.drawRect(x, y, x + cellWidth, y + cellHeight, fillPaint)
                if (textLen > 0 && flags and GhosttyVt.FLAG_INVISIBLE == 0) {
                    val saved = buffer.position()
                    buffer.position(textStart + textOffset * 2)
                    val len = minOf(textLen, chars.size)
                    for (i in 0 until len) {
                        chars[i] = buffer.char
                    }
                    buffer.position(saved)
                    val paint = textPaintFor(flags)
                    paint.color = fg
                    paint.alpha = if (flags and GhosttyVt.FLAG_FAINT != 0) 140 else 255
                    canvas.drawText(chars, 0, len, x, y + baseline, paint)
                    if (flags and GhosttyVt.FLAG_UNDERLINE != 0) {
                        underlinePaint.color = fg
                        underlinePaint.strokeWidth = underlineThickness
                        val uy = y + cellHeight - underlineThickness * 2
                        canvas.drawLine(x, uy, x + cellWidth, uy, underlinePaint)
                    }
                    if (flags and GhosttyVt.FLAG_STRIKETHROUGH != 0) {
                        underlinePaint.color = fg
                        underlinePaint.strokeWidth = underlineThickness
                        val sy = y + cellHeight / 2f
                        canvas.drawLine(x, sy, x + cellWidth, sy, underlinePaint)
                    }
                }
            }
            val textBytes = (textUnits * 2 + 3) and 3.inv()
            buffer.position(textStart + textBytes)
        }
    }

    private fun drawCursor(canvas: Canvas) {
        if (!cursorVisible || cursorX < 0 || cursorY < 0) {
            return
        }
        if (cursorBlinks && !blinkPhaseOn) {
            return
        }
        cursorPaint.color = defaultFg
        val x = cursorX * cellWidth
        val y = cursorY * cellHeight.toFloat()
        when (cursorStyle) {
            GhosttyVt.CURSOR_BAR ->
                canvas.drawRect(x, y, x + underlineThickness * 2, y + cellHeight, cursorPaint)
            GhosttyVt.CURSOR_UNDERLINE ->
                canvas.drawRect(
                    x,
                    y + cellHeight - underlineThickness * 2,
                    x + cellWidth,
                    y + cellHeight,
                    cursorPaint,
                )
            GhosttyVt.CURSOR_BLOCK_HOLLOW -> {
                cursorPaint.style = Paint.Style.STROKE
                cursorPaint.strokeWidth = underlineThickness
                canvas.drawRect(x, y, x + cellWidth, y + cellHeight, cursorPaint)
                cursorPaint.style = Paint.Style.FILL
            }
            else -> canvas.drawRect(x, y, x + cellWidth, y + cellHeight, cursorPaint)
        }
    }

    private fun updateBlink() {
        val shouldBlink = cursorVisible && cursorBlinks
        if (shouldBlink && !blinkScheduled) {
            blinkScheduled = true
            blinkPhaseOn = true
            postDelayed(blinkRunnable, BLINK_INTERVAL_MS)
        } else if (!shouldBlink && blinkScheduled) {
            blinkScheduled = false
            blinkPhaseOn = true
            removeCallbacks(blinkRunnable)
        }
    }

    private fun textPaintFor(flags: Int): Paint {
        val bold = flags and GhosttyVt.FLAG_BOLD != 0
        val italic = flags and GhosttyVt.FLAG_ITALIC != 0
        return when {
            bold && italic -> boldItalicPaint
            bold -> boldPaint
            italic -> italicPaint
            else -> textPaint
        }
    }

    private fun showIme() {
        val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        imm.showSoftInput(this, 0)
    }

    private fun newTextPaint(typeface: Typeface): Paint {
        return Paint(Paint.ANTI_ALIAS_FLAG).apply {
            this.typeface = typeface
            textSize = TypedValue.applyDimension(
                TypedValue.COMPLEX_UNIT_SP,
                FONT_SIZE_SP,
                resources.displayMetrics,
            )
        }
    }

    companion object {
        private const val INITIAL_COLS = 80
        private const val INITIAL_ROWS = 24
        private const val MAX_SCROLLBACK = 10_000L
        private const val FONT_SIZE_SP = 14f
        private const val BLINK_INTERVAL_MS = 530L
        private const val CELL_RECORD_BYTES = 16

        private fun snapshotCapacity(cols: Int, rows: Int): Int {
            val perRow = 4 + cols * CELL_RECORD_BYTES + cols * 8 + 32
            return 12 * 4 + rows * perRow
        }
    }
}

private fun IntArray.toUtf8(): ByteArray {
    val builder = StringBuilder(size)
    for (cp in this) {
        if (cp in 0x20..0x10FFFF && cp != 0x7F) {
            builder.appendCodePoint(cp)
        }
    }
    return builder.toString().toByteArray(Charsets.UTF_8)
}
