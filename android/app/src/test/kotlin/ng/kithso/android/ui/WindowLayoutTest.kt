package ng.kithso.android.ui

import androidx.compose.ui.unit.dp
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class WindowLayoutTest {
    private val tall = 900.dp

    @Test
    fun `under 600dp is the phone layout`() {
        assertEquals(WindowLayout.Phone, windowLayout(599.dp, tall, 1f))
    }

    @Test
    fun `600 to 839dp is the rail layout`() {
        assertEquals(WindowLayout.Rail, windowLayout(600.dp, tall, 1f))
        assertEquals(WindowLayout.Rail, windowLayout(839.dp, tall, 1f))
    }

    @Test
    fun `840dp and wider is the tablet layout`() {
        assertEquals(WindowLayout.Tablet, windowLayout(840.dp, tall, 1f))
        assertEquals(WindowLayout.Tablet, windowLayout(1400.dp, tall, 1f))
    }

    @Test
    fun `the rail is ten items`() {
        // 4, 6 items of 60 with 5 gaps of 4, then 4 items of 60 with 4 gaps of 4 above them, 4
        assertEquals(644f, railHeight(1f).value, 0.01f)
        assertTrue(railHeight(2f) > railHeight(1f))
    }

    @Test
    fun `a window too short for the rail is the phone layout`() {
        assertEquals(WindowLayout.Rail, windowLayout(700.dp, railHeight(1f), 1f))
        assertEquals(WindowLayout.Phone, windowLayout(700.dp, railHeight(1f) - 1.dp, 1f))
        // A phone held sideways.
        assertEquals(WindowLayout.Phone, windowLayout(780.dp, 360.dp, 1f))
    }

    @Test
    fun `a wide window too short for the sidebar takes the narrower layout it's tall enough for`() {
        val sidebar = TopBarHeight + sidebarFixedHeight(1f)
        assertEquals(WindowLayout.Tablet, windowLayout(900.dp, sidebar, 1f))
        // The rail needs more height than the sidebar, so it's the phone layout.
        assertEquals(WindowLayout.Phone, windowLayout(900.dp, sidebar - 1.dp, 1f))
        assertEquals(WindowLayout.Phone, windowLayout(915.dp, 360.dp, 1f))
    }

    @Test
    fun `larger text needs a taller window`() {
        assertEquals(WindowLayout.Rail, windowLayout(700.dp, 700.dp, 1f))
        assertEquals(WindowLayout.Phone, windowLayout(700.dp, 700.dp, 2f))
        assertEquals(WindowLayout.Tablet, windowLayout(900.dp, 600.dp, 1f))
        assertEquals(WindowLayout.Phone, windowLayout(900.dp, 600.dp, 2f))
    }
}
