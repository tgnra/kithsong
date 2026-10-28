package ng.kithso.core

/**
 * musiccore's gapless player (mc_player). It may be used from any thread. [onEvent] is called
 * on the player's own thread with an [Event]; read [status] from there or after posting to
 * the UI thread.
 */
class CorePlayer(private val listener: (Event) -> Unit) : AutoCloseable {
    enum class State { Stopped, Playing, Paused }

    /** Played: a play counts, to take with [takePlayed]. */
    enum class Event { State, Track, Error, Played }

    data class Status(
        val state: State,
        /** Current queue item, -1 if the queue is empty. */
        val index: Int,
        val count: Int,
        val positionMs: Long,
        /** 0 if unknown. */
        val durationMs: Long,
        val shuffle: Boolean,
        /** 0 to 100; see [setVolume]. */
        val volume: Int,
        val muted: Boolean,
    )

    /** A play that counts: queue item [index], begun at [startedMs] (wall clock). */
    data class Played(val index: Int, val startedMs: Long)

    private var handle = nativeNew(this)

    /** Replaces the queue with [paths] and plays item [start], or with -1 the first (shuffled, any). */
    fun setQueue(paths: List<String>, start: Int) =
        nativeSetQueue(handle, paths.toTypedArray(), start)

    /**
     * As [setQueue], but stopped on item [start] (or with -1, the first, which shuffled is any),
     * to play from its start on [resume]. So what will play can be shown before it does.
     */
    fun loadQueue(paths: List<String>, start: Int) =
        nativeLoadQueue(handle, paths.toTypedArray(), start)

    fun skipTo(index: Int) = nativeSkipTo(handle, index)
    fun toggle() = nativeCommand(handle, 0)
    fun pause() = nativeCommand(handle, 1)
    fun resume() = nativeCommand(handle, 2)
    fun stop() = nativeCommand(handle, 3)
    /** After the last item, stops with the first item current. */
    fun next() = nativeCommand(handle, 4)
    /** Back to the start of the item, or to the previous one within its first 3 seconds. */
    fun previous() = nativeCommand(handle, 5)
    fun seekMs(positionMs: Long) = nativeSeekMs(handle, positionMs)

    /**
     * Shuffle (mc_player_set_shuffle): the queue plays in a shuffled order that [next] and
     * [previous] follow, and never ends. Set before [setQueue] to start a queue shuffled.
     */
    fun setShuffle(on: Boolean) = nativeSetShuffle(handle, on)

    /**
     * The play order around the current item (mc_player_order): the queue items at the
     * [count] places from [from] on, counted from the current one (0 is the current item, -1
     * the one before it, 1 the next), -1 where there's none. Shuffled it goes on in new rounds,
     * so an item can come up more than once.
     */
    fun order(from: Int, count: Int): IntArray = nativeOrder(handle, from, count)

    /**
     * The volume, 0 to 100 (100 to begin with), on a perceptual curve; changes fade over a few
     * milliseconds. The system's own volume is left alone.
     */
    fun setVolume(volume: Int) = nativeSetVolume(handle, volume)
    /** Muted is silence, keeping the volume for when it's unmuted. */
    fun setMuted(muted: Boolean) = nativeSetMuted(handle, muted)

    val status: Status
        get() {
            val v = LongArray(8)
            nativeStatus(handle, v)
            return Status(
                State.entries[v[0].toInt()], v[1].toInt(), v[2].toInt(), v[3], v[4],
                shuffle = v[5] != 0L, volume = v[6].toInt(), muted = v[7] != 0L,
            )
        }

    /**
     * The oldest play that has counted since the last call, or null. Plays are dropped with
     * the queue, so take them before [setQueue].
     */
    fun takePlayed(): Played? {
        val v = LongArray(2)
        return if (nativeTakePlayed(handle, v)) Played(v[0].toInt(), v[1]) else null
    }

    /** Stops playback and closes the audio device. */
    override fun close() {
        if (handle != 0L) nativeFree(handle)
        handle = 0
    }

    // Called from jni_bridge.c.
    @Suppress("unused")
    private fun onEvent(event: Int) = listener(Event.entries[event])

    private companion object {
        init {
            System.loadLibrary("kithsong_jni")
        }

        @JvmStatic external fun nativeNew(listener: CorePlayer): Long
        @JvmStatic external fun nativeFree(handle: Long)
        @JvmStatic external fun nativeSetQueue(handle: Long, paths: Array<String>, start: Int)
        @JvmStatic external fun nativeLoadQueue(handle: Long, paths: Array<String>, start: Int)
        @JvmStatic external fun nativeCommand(handle: Long, command: Int)
        @JvmStatic external fun nativeSkipTo(handle: Long, index: Int)
        @JvmStatic external fun nativeSeekMs(handle: Long, ms: Long)
        @JvmStatic external fun nativeSetShuffle(handle: Long, on: Boolean)
        @JvmStatic external fun nativeOrder(handle: Long, from: Int, count: Int): IntArray
        @JvmStatic external fun nativeSetVolume(handle: Long, volume: Int)
        @JvmStatic external fun nativeSetMuted(handle: Long, muted: Boolean)
        @JvmStatic external fun nativeStatus(handle: Long, out: LongArray)
        @JvmStatic external fun nativeTakePlayed(handle: Long, out: LongArray): Boolean

    }
}
