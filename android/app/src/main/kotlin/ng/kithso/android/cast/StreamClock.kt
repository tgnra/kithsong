package ng.kithso.android.cast

/**
 * Keeps a stream of sound in step with real time. Silence fills in once nothing has come for
 * [quietMs], up to where the clock says the stream should be, so gaps too short to fill when they
 * came (a skip's, say) are made up then too. After a gap of more than a second (a phone that
 * slept), only a second is filled, and the stream goes on from there.
 */
class StreamClock(private val quietMs: Long, startMs: Long, private val rate: Int = STREAM_RATE) {
    // When the stream's frame 0 is.
    private var originMs = startMs.toDouble()
    private var soundMs = startMs

    /** Frames of the stream so far. */
    var frames = 0L
        private set

    /** [count] frames of sound went out at [nowMs]. */
    fun sound(count: Int, nowMs: Long) {
        frames += count
        soundMs = nowMs
    }

    /** Frames of silence to write at [nowMs], counted as written. */
    fun silence(nowMs: Long): Int {
        if (nowMs - soundMs < quietMs) return 0
        var due = (nowMs - originMs) * rate / 1000 - frames
        if (due > rate) {
            originMs += (due - rate) * 1000 / rate
            due = rate.toDouble()
        }
        val count = due.toInt().coerceAtLeast(0)
        frames += count
        return count
    }
}
