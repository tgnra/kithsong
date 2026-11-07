package ng.kithso.android.ui

import androidx.compose.ui.unit.dp
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class SidebarTest {
    @Test
    fun `the fixed part is the pages and the three rows under the divider`() {
        // 8 + 6 rows of 44 + 8, the divider, 8 + 3 rows of 44 + 8
        assertEquals(429f, sidebarFixedHeight(1f).value, 0.01f)
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
    fun `recents fill the room Devices leaves`() {
        // Recents' heading 36, Devices' 48 and one device row of 44 leave 400 - 128 = 272: 6 rows.
        assertEquals(6, sidebarRecentsShown(400.dp, 1f, available = 40, devices = 1, nearby = false))
        // In 500, three devices and the Nearby heading (48 + 132 + 36) leave 248: 5 rows.
        assertEquals(5, sidebarRecentsShown(500.dp, 1f, available = 40, devices = 3, nearby = true))
    }

    @Test
    fun `at least three recents show however little room there is`() {
        assertEquals(MIN_SIDEBAR_RECENTS, sidebarRecentsShown(0.dp, 1f, available = 40, devices = 1, nearby = false))
        assertEquals(MIN_SIDEBAR_RECENTS, sidebarRecentsShown(150.dp, 1f, available = 40, devices = 5, nearby = true))
    }

    @Test
    fun `no more recents show than there are`() {
        assertEquals(2, sidebarRecentsShown(1000.dp, 1f, available = 2, devices = 1, nearby = false))
        assertEquals(0, sidebarRecentsShown(1000.dp, 1f, available = 0, devices = 1, nearby = false))
    }

    @Test
    fun `there is no other limit`() {
        // 1000 less the headings and one device leaves 872: 19 rows, more than the phone shows.
        assertEquals(19, sidebarRecentsShown(1000.dp, 1f, available = 40, devices = 1, nearby = false))
    }
}
