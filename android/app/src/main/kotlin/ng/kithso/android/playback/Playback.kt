package ng.kithso.android.playback

import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import ng.kithso.android.data.Track
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch

/**
 * What the UI needs from the player. The properties are snapshot state, so reading one in a
 * composable recomposes it when it changes; read [positionMs] in as narrow a scope as possible,
 * since it changes several times a second.
 */
@Stable
interface Playback {
    val track: Track?
    val isPlaying: Boolean
    val positionMs: Long
    /** Whether the queue goes on after [track]: always, shuffled. */
    val hasNext: Boolean
    /** Whether the queue plays in a shuffled order (the core's, see CoreMediaPlayer). */
    val shuffle: Boolean

    /**
     * Plays [queue] from [index], or with -1 from the first track, which shuffled is any (the
     * core picks it, as the desktops' Shuffle buttons have it). Tracks without a file on this
     * device are left out.
     */
    fun play(queue: List<Track>, index: Int)
    fun toggle()
    fun seekTo(positionMs: Long)
    fun next()
    /** Back to the start of the track, or to the one before within its first 3 seconds. */
    fun previous()
    /** Turns shuffle on or off, carrying on from the current track; set it before [play] to start shuffled. */
    fun setShuffle(on: Boolean)

    /** The uids of the queue's tracks, in queue order. */
    val queueUids: List<String>

    /** Stops and empties the queue, so nothing is playing: before its tracks are deleted, say. */
    fun clear()
}

/** Pretends to play: the position advances while playing and the queue moves on. */
class FakePlayback(private val scope: CoroutineScope) : Playback {
    private var queue: List<Track> = emptyList()
    private var index = 0
    private var ticker: Job? = null

    override var track by mutableStateOf<Track?>(null)
        private set
    override var isPlaying by mutableStateOf(false)
        private set
    override var positionMs by mutableLongStateOf(0L)
        private set
    override val hasNext: Boolean
        get() = track != null && index < queue.lastIndex
    // Only the flag: the pretend queue doesn't shuffle.
    private var shuffleOn by mutableStateOf(false)
    override val shuffle get() = shuffleOn

    /** Loads [queue] at [index] without playing, as if restored from last time. */
    fun load(queue: List<Track>, index: Int) {
        this.queue = queue
        this.index = index
        track = queue.getOrNull(index)
        positionMs = 0
    }

    override fun play(queue: List<Track>, index: Int) {
        load(queue, index.coerceAtLeast(0))
        playOrPause(track != null)
    }

    override fun toggle() = playOrPause(!isPlaying && track != null)

    override fun seekTo(positionMs: Long) {
        this.positionMs = positionMs.coerceIn(0, track?.durationMs ?: 0)
    }

    override fun next() {
        if (hasNext) load(queue, index + 1)
    }

    override fun previous() {
        if (positionMs < 3000 && index > 0) load(queue, index - 1) else positionMs = 0
    }

    override fun setShuffle(on: Boolean) {
        shuffleOn = on
    }

    override val queueUids: List<String> get() = queue.map { it.uid }

    override fun clear() {
        playOrPause(false)
        load(emptyList(), 0)
    }

    private fun playOrPause(playing: Boolean) {
        isPlaying = playing
        ticker?.cancel()
        if (!playing) return
        ticker = scope.launch {
            while (isActive) {
                delay(TICK_MS)
                val current = track ?: break
                positionMs += TICK_MS
                if (positionMs >= current.durationMs) {
                    if (index + 1 < queue.size) load(queue, index + 1) else break
                }
            }
            isPlaying = false
        }
    }

    private companion object {
        const val TICK_MS = 250L
    }
}
