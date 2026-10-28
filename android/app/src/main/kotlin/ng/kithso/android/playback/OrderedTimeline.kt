package ng.kithso.android.playback

import androidx.media3.common.C
import androidx.media3.common.Player
import androidx.media3.common.Timeline

/**
 * [timeline] with [shuffled] as its shuffle order: the queue indexes in the order they play
 * shuffled, each once. Media3 has no other way to be told a player's own shuffled order; with
 * it, Media3 and every controller (the app's UI, the notification) know which item comes next
 * and before, so skipping shows the right track straight away.
 */
class OrderedTimeline(private val timeline: Timeline, private val shuffled: IntArray) : Timeline() {
    // Each queue index's place in [shuffled].
    private val places = IntArray(shuffled.size).also { places -> shuffled.forEachIndexed { place, i -> places[i] = place } }

    init {
        require(shuffled.size == timeline.windowCount)
    }

    override fun getWindowCount() = timeline.windowCount
    override fun getWindow(windowIndex: Int, window: Window, defaultPositionProjectionUs: Long): Window =
        timeline.getWindow(windowIndex, window, defaultPositionProjectionUs)
    override fun getPeriodCount() = timeline.periodCount
    override fun getPeriod(periodIndex: Int, period: Period, setIds: Boolean): Period = timeline.getPeriod(periodIndex, period, setIds)
    override fun getIndexOfPeriod(uid: Any) = timeline.getIndexOfPeriod(uid)
    override fun getUidOfPeriod(periodIndex: Int): Any = timeline.getUidOfPeriod(periodIndex)

    override fun getFirstWindowIndex(shuffleModeEnabled: Boolean): Int = when {
        isEmpty -> C.INDEX_UNSET
        shuffleModeEnabled -> shuffled.first()
        else -> timeline.getFirstWindowIndex(false)
    }

    override fun getLastWindowIndex(shuffleModeEnabled: Boolean): Int = when {
        isEmpty -> C.INDEX_UNSET
        shuffleModeEnabled -> shuffled.last()
        else -> timeline.getLastWindowIndex(false)
    }

    override fun getNextWindowIndex(windowIndex: Int, repeatMode: Int, shuffleModeEnabled: Boolean): Int {
        if (!shuffleModeEnabled) return timeline.getNextWindowIndex(windowIndex, repeatMode, false)
        val place = places[windowIndex]
        return when {
            repeatMode == Player.REPEAT_MODE_ONE -> windowIndex
            place < shuffled.lastIndex -> shuffled[place + 1]
            repeatMode == Player.REPEAT_MODE_ALL -> shuffled.first()
            else -> C.INDEX_UNSET
        }
    }

    override fun getPreviousWindowIndex(windowIndex: Int, repeatMode: Int, shuffleModeEnabled: Boolean): Int {
        if (!shuffleModeEnabled) return timeline.getPreviousWindowIndex(windowIndex, repeatMode, false)
        val place = places[windowIndex]
        return when {
            repeatMode == Player.REPEAT_MODE_ONE -> windowIndex
            place > 0 -> shuffled[place - 1]
            repeatMode == Player.REPEAT_MODE_ALL -> shuffled.last()
            else -> C.INDEX_UNSET
        }
    }
}
