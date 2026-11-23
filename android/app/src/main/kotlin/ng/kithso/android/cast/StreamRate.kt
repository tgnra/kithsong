package ng.kithso.android.cast

/**
 * How much the network to the Cast device can take, and so which of [levels] the stream should be
 * at: the bits a second of each, most first. It goes by how fast segments get to the device
 * ([sent]) and by the device running dry ([stalled]).
 *
 * Coming down is quick: straight to the first level that fits. Going up again is one level at a
 * time, and only after a while with much more room than that needs; having to come down again
 * makes the next wait longer. [tooSlow] is for a network that can't take the last level either,
 * which is decided slowly, as there's no way back from it.
 *
 * Times are in ms, from any start. Safe from any thread.
 */
class StreamRate(private val levels: List<Int>, startMs: Long) {
    private class Sample(val bytes: Long, val ms: Long, val atMs: Long)

    private val samples = ArrayDeque<Sample>()
    private val stalls = ArrayDeque<Long>()
    private var changedMs = startMs
    private var upWaitMs = UP_WAIT_MS
    private var wentUp = false

    /** The level to be at, in [levels]. */
    @get:Synchronized
    var level = 0
        private set

    @get:Synchronized
    var tooSlow = false
        private set

    /**
     * [bytes] took [ms] to get to the device, finished at [nowMs]. A transfer still under way can
     * be given too ([unfinished]), for what has gone so far.
     */
    @Synchronized
    fun sent(bytes: Long, ms: Long, nowMs: Long, unfinished: Boolean = false) {
        samples += Sample(bytes, ms.coerceAtLeast(1), nowMs)
        while (samples.size > SAMPLES || samples.first().atMs < nowMs - SAMPLE_AGE_MS) samples.removeFirst()
        // One is enough if it's that slow.
        if (samples.size < SAMPLES && !unfinished) return
        val rate = samples.sumOf { it.bytes } * 8000 / samples.sumOf { it.ms }
        val settled = nowMs - changedMs
        when {
            level < levels.lastIndex && rate < levels[level] * ROOM -> {
                val fits = (level + 1..levels.lastIndex).firstOrNull { rate >= levels[it] * ROOM }
                down(fits ?: levels.lastIndex, nowMs)
            }
            level == levels.lastIndex && rate < levels[level] * LAST_ROOM && settled >= SETTLE_MS && samples.size >= SAMPLES -> tooSlow = true
            level > 0 && settled >= upWaitMs && rate > levels[level - 1] * UP_ROOM && stalls.none { it > nowMs - UP_WAIT_MS } -> {
                level--
                changedMs = nowMs
                wentUp = true
            }
        }
    }

    /** The device ran out of stream to play, at [nowMs]. Once may be chance, or what was sent before a change. */
    @Synchronized
    fun stalled(nowMs: Long) {
        if (nowMs - changedMs < SETTLE_MS) return
        stalls += nowMs
        while (stalls.first() < nowMs - STALL_AGE_MS) stalls.removeFirst()
        if (stalls.size < STALLS) return
        if (level < levels.lastIndex) down(level + 1, nowMs) else tooSlow = true
    }

    private fun down(to: Int, nowMs: Long) {
        level = to
        changedMs = nowMs
        stalls.clear()
        if (wentUp) upWaitMs = (upWaitMs * 2).coerceAtMost(UP_WAIT_MOST_MS)
        wentUp = false
    }

    private companion object {
        const val SAMPLES = 3
        const val SAMPLE_AGE_MS = 30_000L
        // How much faster than a level the network has to be for it, as neither is steady.
        const val ROOM = 1.4
        const val LAST_ROOM = 1.1
        const val UP_ROOM = 2.5
        // What the device has already is played before a change is heard of: over the six
        // seconds it's behind.
        const val SETTLE_MS = 15_000L
        const val STALLS = 2
        const val STALL_AGE_MS = 60_000L
        const val UP_WAIT_MS = 60_000L
        const val UP_WAIT_MOST_MS = 10 * 60_000L
    }
}
