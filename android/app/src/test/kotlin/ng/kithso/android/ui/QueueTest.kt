package ng.kithso.android.ui

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class QueueTest {
    @Test
    fun `a slow release goes by whether it's past halfway`() {
        assertFalse(queueOpensOnRelease(0.4f, -100f, FLING))
        assertTrue(queueOpensOnRelease(0.6f, 100f, FLING))
    }

    @Test
    fun `a fling goes the way it's flung, wherever it is`() {
        assertTrue(queueOpensOnRelease(0.1f, -FLING, FLING))
        assertFalse(queueOpensOnRelease(0.9f, FLING, FLING))
    }

    @Test
    fun `moved puts an item at its new place`() {
        assertEquals(listOf("b", "c", "a"), listOf("a", "b", "c").moved(0, 2))
        assertEquals(listOf("c", "a", "b"), listOf("a", "b", "c").moved(2, 0))
        assertEquals(listOf("a", "b", "c"), listOf("a", "b", "c").moved(1, 1))
    }

    private companion object {
        const val FLING = 1000f
    }
}
