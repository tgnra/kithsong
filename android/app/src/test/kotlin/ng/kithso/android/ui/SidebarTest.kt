package ng.kithso.android.ui

import androidx.compose.ui.unit.dp
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SidebarTest {
    @Test
    fun `the fixed part is the pages and the four rows under the divider`() {
        // 8 + 6 rows of 44 + 8, the divider, 8 + 4 rows of 44 + 8
        assertEquals(473f, sidebarFixedHeight(1f).value, 0.01f)
    }

    @Test
    fun `the Galaxy Z Fold 8 open has room for the tablet layout`() {
        // 1848px at 420dpi, less the status bar's 24dp and the gesture bar's 16dp.
        val height = (1848 / 2.625f).dp - 24.dp - 16.dp
        assertTrue(height >= TopBarHeight + sidebarFixedHeight(1f))
    }

    @Test
    fun `larger text makes the fixed part taller`() {
        assertTrue(sidebarFixedHeight(2f) > sidebarFixedHeight(1f))
    }

    @Test
    fun `recents fill the room there is`() {
        // Recents' heading 36 leaves 400 - 36 = 364: 8 rows.
        assertEquals(8, sidebarRecentsShown(400.dp, 1f, available = 40))
    }

    @Test
    fun `at least one recent shows however little room there is`() {
        assertEquals(MIN_SIDEBAR_RECENTS, sidebarRecentsShown(0.dp, 1f, available = 40))
    }

    @Test
    fun `no more recents show than there are`() {
        assertEquals(2, sidebarRecentsShown(1000.dp, 1f, available = 2))
        assertEquals(0, sidebarRecentsShown(1000.dp, 1f, available = 0))
    }
}
