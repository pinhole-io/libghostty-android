package com.example.harn.terminal

import android.os.ParcelFileDescriptor
import java.io.FileInputStream
import java.io.FileOutputStream
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Local PTY attached to `/system/bin/sh`.
 *
 * Who can reach this: the device user of this app.
 * Worst input: shell commands as the app UID.
 * Failure leak: local process errors. No secrets.
 */
internal class LocalPty(
    private val masterFd: Int,
    private val pid: Int,
) : AutoCloseable {
    private val pfd = ParcelFileDescriptor.adoptFd(masterFd)
    private val input = FileInputStream(pfd.fileDescriptor)
    private val output = FileOutputStream(pfd.fileDescriptor)
    private val closed = AtomicBoolean(false)

    fun write(bytes: ByteArray) {
        if (closed.get() || bytes.isEmpty()) {
            return
        }
        output.write(bytes)
        output.flush()
    }

    fun read(buffer: ByteArray): Int {
        if (closed.get()) {
            return -1
        }
        return input.read(buffer)
    }

    fun setWindowSize(cols: Int, rows: Int, cellWidthPx: Int, cellHeightPx: Int) {
        if (!closed.get()) {
            nativeSetWindowSize(masterFd, cols, rows, cellWidthPx, cellHeightPx)
        }
    }

    override fun close() {
        if (!closed.compareAndSet(false, true)) {
            return
        }
        nativeClose(pid)
        runCatching { pfd.close() }
    }

    companion object {
        init {
            System.loadLibrary("harn_ghostty")
        }

        fun start(
            cwd: String,
            shell: String,
            env: Array<String>,
            cols: Int,
            rows: Int,
            cellWidthPx: Int,
            cellHeightPx: Int,
        ): LocalPty? {
            val ids = nativeStart(cwd, shell, env, cols, rows, cellWidthPx, cellHeightPx)
                ?: return null
            if (ids.size < 2 || ids[0] < 0 || ids[1] <= 0) {
                return null
            }
            return LocalPty(ids[0], ids[1])
        }

        @JvmStatic
        private external fun nativeStart(
            cwd: String,
            shell: String,
            env: Array<String>,
            cols: Int,
            rows: Int,
            cellWidthPx: Int,
            cellHeightPx: Int,
        ): IntArray?

        @JvmStatic
        private external fun nativeSetWindowSize(
            fd: Int,
            cols: Int,
            rows: Int,
            cellWidthPx: Int,
            cellHeightPx: Int,
        )

        @JvmStatic
        private external fun nativeClose(pid: Int)
    }
}
