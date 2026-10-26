package app.musicapp

/**
 * musiccore's gapless player (mc_player). It may be used from any thread. [onEvent] is called
 * on the player's own thread with an [Event]; read [status] from there or after posting to
 * the UI thread.
 */
class CorePlayer(private val listener: (Event) -> Unit) : AutoCloseable {
    enum class State { Stopped, Playing, Paused }

    enum class Event { State, Track, Error }

    data class Status(
        val state: State,
        /** Current queue item, -1 if the queue is empty. */
        val index: Int,
        val count: Int,
        val positionMs: Long,
        /** 0 if unknown. */
        val durationMs: Long,
    )

    private var handle = nativeNew(this)

    /** Replaces the queue with [paths] and plays item [start]. */
    fun setQueue(paths: List<String>, start: Int) =
        nativeSetQueue(handle, paths.toTypedArray(), start)

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

    val status: Status
        get() {
            val v = LongArray(5)
            nativeStatus(handle, v)
            return Status(State.entries[v[0].toInt()], v[1].toInt(), v[2].toInt(), v[3], v[4])
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
            System.loadLibrary("musicapp_jni")
        }

        @JvmStatic external fun nativeNew(listener: CorePlayer): Long
        @JvmStatic external fun nativeFree(handle: Long)
        @JvmStatic external fun nativeSetQueue(handle: Long, paths: Array<String>, start: Int)
        @JvmStatic external fun nativeCommand(handle: Long, command: Int)
        @JvmStatic external fun nativeSkipTo(handle: Long, index: Int)
        @JvmStatic external fun nativeSeekMs(handle: Long, ms: Long)
        @JvmStatic external fun nativeStatus(handle: Long, out: LongArray)
    }
}
