package ng.kithso.android.playback

import ng.kithso.core.CorePlayer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class SavedQueueTest {
    private val album = CorePlayer.Origin(CorePlayer.PlayContext.Album, "album-uid")
    private val queue = SavedQueue(listOf("a", "b", "c", "d"), 2, 61_000, album)

    @Test
    fun readsBackWhatItWrote() {
        assertEquals(queue, SavedQueue.decode(queue.encode()))
    }

    @Test
    fun anOriginWithNoUid() {
        val manual = queue.copy(origin = CorePlayer.Origin(CorePlayer.PlayContext.Manual))
        assertEquals(manual, SavedQueue.decode(manual.encode()))
    }

    @Test
    fun whatIsntAQueueIsNone() {
        assertNull(SavedQueue.decode(""))
        assertNull(SavedQueue.decode("0 0 2 x\n"))
        assertNull(SavedQueue.decode("9 0 2 x\na\n"))
        assertNull(SavedQueue.decode("not a queue at all\na\n"))
    }

    @Test
    fun aTrackGoneBeforeTheOnePlayingKeepsThePlace() {
        val left = queue.keeping { it != 0 }!!
        assertEquals(listOf("b", "c", "d"), left.uids)
        assertEquals(1, left.index)
        assertEquals(61_000, left.positionMs)
    }

    @Test
    fun theTrackPlayingGoneGoesOnToTheNext() {
        val left = queue.keeping { it != 2 }!!
        assertEquals(listOf("a", "b", "d"), left.uids)
        assertEquals(2, left.index)
        assertEquals(0, left.positionMs)
    }

    @Test
    fun theLastTrackGoneFallsBackToTheOneBefore() {
        val left = queue.copy(index = 3).keeping { it != 3 }!!
        assertEquals(2, left.index)
    }

    @Test
    fun nothingLeftIsNoQueue() {
        assertNull(queue.keeping { false })
    }
}
