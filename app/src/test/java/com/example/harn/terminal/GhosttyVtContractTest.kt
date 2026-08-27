package com.example.harn.terminal

import org.junit.Assert.assertEquals
import org.junit.Test

class GhosttyVtContractTest {
    @Test
    fun snapshotVersionMatchesNativeBridge() {
        assertEquals(1, GhosttyVt.SNAPSHOT_VERSION)
    }

    @Test
    fun cellStyleFlagsAreDistinctBits() {
        val flags = intArrayOf(
            GhosttyVt.FLAG_BOLD,
            GhosttyVt.FLAG_ITALIC,
            GhosttyVt.FLAG_FAINT,
            GhosttyVt.FLAG_UNDERLINE,
            GhosttyVt.FLAG_STRIKETHROUGH,
            GhosttyVt.FLAG_INVERSE,
            GhosttyVt.FLAG_INVISIBLE,
            GhosttyVt.FLAG_FG_DEFAULT,
            GhosttyVt.FLAG_BG_NONE,
        )
        val combined = flags.reduce { acc, flag -> acc or flag }
        assertEquals(flags.size, Integer.bitCount(combined))
    }
}
