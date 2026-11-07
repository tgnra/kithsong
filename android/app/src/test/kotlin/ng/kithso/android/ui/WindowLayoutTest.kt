package ng.kithso.android.ui

import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.android.ui.WindowLayoutClass.Compact
import ng.kithso.android.ui.WindowLayoutClass.Large
import ng.kithso.android.ui.WindowLayoutClass.Medium
import ng.kithso.android.ui.WindowLayoutClass.Small
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class WindowLayoutTest {
    private fun classOf(width: Int, height: Int, fold: WindowFold? = null, foldKnown: Boolean = true) =
        windowLayoutClass(width.dp, height.dp, fold, foldKnown)

    private fun layout(width: Int, height: Int, fold: WindowFold? = null) = WindowLayout.of(width.dp, height.dp, fold)

    private fun hinge(at: Dp) = WindowFold(vertical = true, start = at, end = at)

    @Test
    fun aPhoneInPortraitIsCompactAndNotDense() {
        val phone = layout(412, 915)
        assertEquals(Compact, phone.layoutClass)
        assertFalse(phone.dense)
        assertEquals(LayoutScale.Regular, phone.scale)
    }

    @Test
    fun aPhoneInLandscapeIsMediumAndDense() {
        for ((width, height) in listOf(915 to 412, 640 to 360)) {
            val phone = layout(width, height)
            assertEquals(Medium, phone.layoutClass)
            assertTrue(phone.dense)
            assertEquals(LayoutScale.Dense, phone.scale)
        }
    }

    @Test
    fun theJellyIsSmallAndDense() {
        val jelly = layout(240, 427)
        assertEquals(Small, jelly.layoutClass)
        assertTrue(jelly.dense)
    }

    @Test
    fun smallIsOnlyUnderMediumsWidth() {
        assertEquals(Small, classOf(339, 800))
        assertEquals(Compact, classOf(340, 800))
        assertEquals(Small, classOf(599, 479))
        assertEquals(Compact, classOf(599, 480))
        assertEquals(Medium, classOf(600, 479))
        // The Z Flip's cover, and the SP-01.
        assertEquals(Small, classOf(316, 349))
        assertEquals(Small, classOf(274, 366))
    }

    @Test
    fun smallIsAlwaysDense() {
        // Tall, but narrow.
        assertTrue(layout(320, 800).dense)
    }

    @Test
    fun denseGoesByHeightAtAnyWidth() {
        assertTrue(layout(1000, 479).dense)
        assertFalse(layout(1000, 480).dense)
    }

    @Test
    fun tablets() {
        // Store_Tablet_7, 1080x1920 at 288 dpi.
        assertEquals(Medium, classOf(600, 1067))
        assertEquals(Large, classOf(1067, 600))
        // Store_Tablet_10, 1440x2560 at 288 dpi.
        assertEquals(Large, classOf(800, 1422))
        assertEquals(Large, classOf(1422, 800))
    }

    @Test
    fun aThresholdIsMetToTheNearestDp() {
        // Store_Tablet_10 as measured: 1440 pixels at a density of 1.8.
        assertEquals(Large, windowLayoutClass(799.99994.dp, 1422.2222.dp))
        assertEquals(Medium, windowLayoutClass(599.99994.dp, 1066.6666.dp))
        assertFalse(WindowLayout.of(1000.dp, 479.99994.dp).dense)
    }

    @Test
    fun largeNeedsItsHeight() {
        assertEquals(Large, classOf(800, 560))
        assertEquals(Medium, classOf(800, 559))
        assertEquals(Medium, classOf(1400, 559))
        assertEquals(Medium, classOf(799, 1000))
    }

    @Test
    fun aFoldIsNeverLarge() {
        // The Z Fold 8 open at 420 and 480 dpi.
        assertEquals(Medium, classOf(933, 704, hinge(466.dp)))
        assertEquals(Medium, classOf(816, 616, hinge(408.dp)))
        // Without its fold 933x704 would be Large, so the fold is what makes it Medium.
        assertEquals(Large, classOf(933, 704))
        // Half open is the same feature, and across the window it may lie either way.
        assertEquals(Medium, classOf(704, 933, WindowFold(vertical = false, start = 466.dp, end = 466.dp)))
        assertEquals(Medium, classOf(933, 704, WindowFold(vertical = false, start = 352.dp, end = 352.dp)))
    }

    @Test
    fun aFoldCoverStaysCompact() {
        assertEquals(Compact, classOf(475, 751))
    }

    @Test
    fun notYetKnowingOfAFoldIsNeverLarge() {
        assertEquals(Medium, classOf(933, 704, foldKnown = false))
        assertEquals(Compact, classOf(412, 915, foldKnown = false))
    }

    @Test
    fun aFoldOutlastsItsReportUntilTheWindowResizes() {
        val memory = FoldMemory<String>()
        val fold = hinge(466.dp)
        assertEquals(null, memory.resolve("open", null))
        assertEquals(fold, memory.resolve("open", fold))
        // Folding: the report goes first, while the window is still as wide as a tablet.
        assertEquals(fold, memory.resolve("open", null))
        assertEquals(null, memory.resolve("cover", null))
        // Back at that size it has the fold still, until a report says otherwise.
        assertEquals(fold, memory.resolve("open", null))
    }

    @Test
    fun compactAndSmallDrawEverythingOverTheWholeWindow() {
        for (window in listOf(layout(412, 915), layout(240, 427))) {
            val panes = panesFor(window)
            assertFalse(panes.sideBySide)
            for (area in listOf(panes.main, panes.pages, panes.miniPlayer, panes.player)) {
                assertTrue(area.whole)
                assertEquals(0.dp, area.x)
                assertEquals(window.width, area.width)
                assertEquals(window.height, area.height)
            }
        }
    }

    @Test
    fun mediumIsHalvesWithPagesAndPlayersOnTheRight() {
        val panes = panesFor(layout(915, 412))
        assertTrue(panes.sideBySide)
        assertEquals(0.dp, panes.main.x)
        assertEquals(457.5.dp, panes.main.width)
        assertEquals(457.5.dp, panes.pages.x)
        assertEquals(457.5.dp, panes.pages.width)
        assertEquals(panes.pages, panes.miniPlayer)
        assertEquals(panes.pages, panes.player)
    }

    @Test
    fun panesGoEitherSideOfAHinge() {
        val panes = panesFor(layout(933, 704, WindowFold(vertical = true, start = 460.dp, end = 480.dp)))
        assertEquals(460.dp, panes.main.width)
        assertEquals(480.dp, panes.pages.x)
        assertEquals(453.dp, panes.pages.width)
    }

    @Test
    fun aFoldAcrossTheWindowLeavesHalves() {
        val panes = panesFor(layout(704, 933, WindowFold(vertical = false, start = 460.dp, end = 470.dp)))
        assertEquals(352.dp, panes.main.width)
        assertEquals(352.dp, panes.pages.x)
    }

    @Test
    fun aPaneOnlyClearsTheEdgesItTouches() {
        for (window in listOf(layout(915, 412), layout(1422, 800))) {
            val panes = panesFor(window)
            assertEquals(WindowInsetsSides.Left + WindowInsetsSides.Vertical, panes.main.sides)
            assertEquals(WindowInsetsSides.Right + WindowInsetsSides.Vertical, panes.pages.sides)
        }
    }

    @Test
    fun largesPanesFillTheWindow() {
        val panes = panesFor(layout(1000, 800))
        assertEquals(panes.main.width, panes.pages.x)
        assertEquals(1000.dp, panes.pages.x + panes.pages.width)
    }

    @Test
    fun rightToLeftMirrorsThePanes() {
        val hinged = layout(1000, 800, hinge(450.dp))
        val panes = panesFor(hinged, rtl = true)
        assertEquals(450.dp, panes.main.x)
        assertEquals(550.dp, panes.main.width)
        assertEquals(0.dp, panes.pages.x)
        assertEquals(450.dp, panes.pages.width)
        assertEquals(panes.pages, panes.miniPlayer)
        assertEquals(WindowInsetsSides.Right + WindowInsetsSides.Vertical, panes.main.sides)
    }

    @Test
    fun onlyMediumsHomeIsColumns() {
        assertTrue(panesFor(layout(915, 412)).columns)
        assertFalse(panesFor(layout(412, 915)).columns)
        assertFalse(panesFor(layout(1422, 800)).columns)
    }

    @Test
    fun theStripsColumnsMoveByTheirPanes() {
        val strip = homeStrip(panesFor(layout(700, 800)))
        assertEquals(350.dp, strip.middle.x)
        assertEquals(350.dp, strip.middle.width)
        assertEquals(350.dp, strip.middleContent)
        assertEquals((-350).dp, strip.leftMove)
        assertEquals((-350).dp, strip.middleMove)
    }

    @Test
    fun anOffCentreHingeKeepsTheMiddleColumnInsideBothPanes() {
        val panes = panesFor(layout(1000, 800, hinge(450.dp)))
        val strip = homeStrip(panes)
        // The same width in either pane, so what was tapped stays put.
        assertEquals(450.dp, strip.middleContent)
        // At rest, centred in the right pane.
        assertEquals(450.dp, strip.middle.x)
        assertEquals(550.dp, strip.middle.width)
        // Moved, centred in the left one, which it just fits.
        val centre = strip.middle.x + strip.middle.width / 2 + strip.middleMove
        assertEquals(225.dp, centre)
        assertEquals((-450).dp, strip.leftMove)
    }

    @Test
    fun rightToLeftTheStripGoesTheOtherWay() {
        val strip = homeStrip(panesFor(layout(700, 800), rtl = true))
        assertEquals(0.dp, strip.middle.x)
        assertEquals(350.dp, strip.leftMove)
        assertEquals(350.dp, strip.middleMove)
    }

    @Test
    fun theWordmarkGivesWayToKeepTheIcons() {
        assertEquals(HeaderMark.Wordmark(48.dp), headerMark(201.dp))
        // A 7 inch tablet's 300 dp column has 40 dp left beside the five icons.
        assertEquals(HeaderMark.Tile(44.dp), headerMark(40.dp))
        assertEquals(HeaderMark.None, headerMark(10.dp))
    }
}
