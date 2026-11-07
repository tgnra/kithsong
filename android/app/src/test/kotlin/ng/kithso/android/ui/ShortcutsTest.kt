package ng.kithso.android.ui

import android.view.KeyEvent
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ShortcutsTest {
    private fun key(code: Int, ctrl: Boolean = false, shift: Boolean = false, alt: Boolean = false, meta: Boolean = false) =
        shortcutFor(code, ctrl, shift, alt, meta)

    @Test
    fun theDesktopsKeys() {
        assertEquals(Shortcut.Next, key(KeyEvent.KEYCODE_DPAD_RIGHT, ctrl = true))
        assertEquals(Shortcut.Previous, key(KeyEvent.KEYCODE_DPAD_LEFT, ctrl = true))
        assertEquals(Shortcut.SeekForward, key(KeyEvent.KEYCODE_DPAD_RIGHT, shift = true))
        assertEquals(Shortcut.SeekBack, key(KeyEvent.KEYCODE_DPAD_LEFT, shift = true))
        assertEquals(Shortcut.VolumeUp, key(KeyEvent.KEYCODE_DPAD_UP, ctrl = true))
        assertEquals(Shortcut.VolumeDown, key(KeyEvent.KEYCODE_DPAD_DOWN, ctrl = true))
        assertEquals(Shortcut.Mute, key(KeyEvent.KEYCODE_M, ctrl = true))
        assertEquals(Shortcut.Shuffle, key(KeyEvent.KEYCODE_S, ctrl = true))
        assertEquals(Shortcut.Search, key(KeyEvent.KEYCODE_F, ctrl = true))
        assertEquals(Shortcut.Albums, key(KeyEvent.KEYCODE_1, ctrl = true))
        assertEquals(Shortcut.AlbumGroups, key(KeyEvent.KEYCODE_4, ctrl = true))
        assertEquals(Shortcut.Settings, key(KeyEvent.KEYCODE_COMMA, ctrl = true))
        assertEquals(Shortcut.Import, key(KeyEvent.KEYCODE_O, ctrl = true))
        assertEquals(Shortcut.NewPlaylist, key(KeyEvent.KEYCODE_N, ctrl = true))
        assertEquals(Shortcut.Shortcuts, key(KeyEvent.KEYCODE_SLASH, ctrl = true, shift = true))
        assertEquals(Shortcut.PlayPause, key(KeyEvent.KEYCODE_SPACE))
        assertEquals(Shortcut.Back, key(KeyEvent.KEYCODE_ESCAPE))
    }

    @Test
    fun plusAndMinusWithOrWithoutShift() {
        assertEquals(Shortcut.VolumeUp, key(KeyEvent.KEYCODE_EQUALS))
        assertEquals(Shortcut.VolumeUp, key(KeyEvent.KEYCODE_EQUALS, shift = true))
        assertEquals(Shortcut.VolumeUp, key(KeyEvent.KEYCODE_NUMPAD_ADD))
        assertEquals(Shortcut.VolumeDown, key(KeyEvent.KEYCODE_MINUS))
        assertEquals(Shortcut.VolumeDown, key(KeyEvent.KEYCODE_NUMPAD_SUBTRACT))
    }

    @Test
    fun theArrowsAloneAndTheSystemsKeysAreLeftAlone() {
        assertNull(key(KeyEvent.KEYCODE_DPAD_RIGHT))
        assertNull(key(KeyEvent.KEYCODE_DPAD_UP, shift = true))
        assertNull(key(KeyEvent.KEYCODE_F))
        assertNull(key(KeyEvent.KEYCODE_F, ctrl = true, meta = true))
        assertNull(key(KeyEvent.KEYCODE_DPAD_LEFT, alt = true))
        assertNull(key(KeyEvent.KEYCODE_S, ctrl = true, shift = true))
        // Ctrl+R is repeat's, when it comes.
        assertNull(key(KeyEvent.KEYCODE_R, ctrl = true))
    }

    @Test
    fun aTextFieldKeepsItsOwnKeys() {
        for (s in listOf(Shortcut.PlayPause, Shortcut.Next, Shortcut.Previous, Shortcut.SeekForward, Shortcut.VolumeUp)) {
            assertFalse(s.name, s.inText)
        }
        for (s in listOf(Shortcut.Search, Shortcut.Mute, Shortcut.Shuffle, Shortcut.Back)) assertTrue(s.name, s.inText)
        assertTrue(Shortcut.PlayPause.afterFocused)
        assertFalse(Shortcut.Next.afterFocused)
    }
}
