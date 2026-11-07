package ng.kithso.android.ui

import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PlayerLayoutTest {
    private val size = LayoutScale.Dense.size

    // A window of this many real dp, as the player measures it, with the status bar taken off.
    private fun wideDense(width: Int, height: Int) =
        playerIsWide(width.dp / size, height.dp / size - 28.dp, dense = true, fontScale = 1f)

    @Test
    fun aLandscapePhoneHasItsArtBesideTheControls() {
        assertTrue(wideDense(915, 412))
        assertTrue(wideDense(640, 360))
    }

    @Test
    fun aLandscapePhonesHalfPaneLeavesTheControlsTheirWidth() {
        val half: Dp = 460.dp / size
        assertTrue(playerArtMaxBeside(half, dense = true) > 150.dp)
    }

    @Test
    fun theSmallestWindowHasItsArtAboveTheControls() {
        assertFalse(wideDense(320, 360))
    }

    @Test
    fun aNearlySquareRegularPaneHasItsArtAboveTheControls() {
        assertFalse(playerIsWide(490.dp, 482.dp, dense = false, fontScale = 1f))
    }

    @Test
    fun aLandscapeTabletHasItsArtBesideTheControls() {
        assertTrue(playerIsWide(1280.dp, 800.dp, dense = false, fontScale = 1f))
        assertEquals(1280.dp - 48.dp - 32.dp - 296.dp, playerArtMaxBeside(1280.dp, dense = false))
    }

    @Test
    fun aPhoneUprightHasItsArtAboveTheControls() {
        assertFalse(playerIsWide(412.dp, 915.dp, dense = false, fontScale = 1f))
    }
}
