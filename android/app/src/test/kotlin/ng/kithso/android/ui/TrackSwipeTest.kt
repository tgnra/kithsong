package ng.kithso.android.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs

class TrackSwipeTest {
    private val page = 900f
    private val fling = 1000f

    private fun skip(offset: Float, velocity: Float, canNext: Boolean = true, canPrevious: Boolean = true) =
        skipOnRelease(offset, velocity, page, fling, canNext, canPrevious)

    @Test
    fun leftIsNextAndRightIsPrevious() {
        assertEquals(Skip.Next, skip(-400f, 0f))
        assertEquals(Skip.Previous, skip(400f, 0f))
    }

    @Test
    fun aSlowReleaseShortOfAThirdSpringsBack() {
        assertNull(skip(-299f, -100f))
        assertNull(skip(299f, 100f))
    }

    @Test
    fun aFlingSkipsFromAnywhere() {
        assertEquals(Skip.Next, skip(-30f, -1500f))
        assertEquals(Skip.Previous, skip(30f, 1500f))
    }

    @Test
    fun aFlingBackTheWayItCameSpringsBack() {
        assertNull(skip(-600f, 1500f))
        assertNull(skip(600f, -1500f))
    }

    @Test
    fun nothingToSkipToStaysPut() {
        assertNull(skip(-800f, -2000f, canNext = false))
        assertNull(skip(800f, 2000f, canPrevious = false))
    }

    @Test
    fun theOtherWayStillSkipsAtAnEndOfTheQueue() {
        assertEquals(Skip.Previous, skip(800f, 0f, canNext = false))
        assertEquals(Skip.Next, skip(-800f, 0f, canPrevious = false))
    }

    @Test
    fun theRubberBandFollowsTheDragButNeverPastItsLimit() {
        val limit = page * 0.12f
        assertEquals(0f, rubberBand(0f, page), 0f)
        assertTrue(rubberBand(100f, page) < 100f)
        assertTrue(rubberBand(300f, page) > rubberBand(100f, page))
        assertTrue(rubberBand(100000f, page) <= limit)
        assertEquals(rubberBand(250f, page), -rubberBand(-250f, page), 0.001f)
        assertTrue(abs(rubberBand(-100000f, page)) <= limit)
    }

    @Test
    fun theRubberBandHoldsBeforeThereIsAPage() {
        assertEquals(0f, rubberBand(50f, 0f), 0f)
    }
}
