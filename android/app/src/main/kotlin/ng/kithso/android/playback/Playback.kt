package ng.kithso.android.playback

import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Track
import ng.kithso.core.CorePlayer
import ng.kithso.core.UpNextEntry
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
    /** Whether the queue goes on after [track]: always, shuffled or while a station adds to it. */
    val hasNext: Boolean
    /** Whether there's a track before [track]: shuffled, one that played before it. */
    val hasPrevious: Boolean
    /** The track after [track]; null if there's none or, past a shuffled round or on a station, none picked yet. */
    val nextTrack: Track?
    /** The track before [track], or null if there's none. */
    val previousTrack: Track?
    /** Whether the queue plays in a shuffled order (the core's, see CoreMediaPlayer). */
    val shuffle: Boolean
    /** Whether the queue is a radio station's, whose own order shuffle doesn't change. */
    val station: Boolean get() = false

    /**
     * Plays [queue] from [index], or with -1 from the first track, which shuffled is any (the
     * core picks it, as the desktops' Shuffle buttons have it). Tracks without a file on this
     * device are left out. [origin] says what's played, for the play log. One track with the
     * radio as its [origin] is a station from that track, whose queue grows as it plays.
     */
    fun play(queue: List<Track>, index: Int, origin: CorePlayer.Origin)
    fun toggle()
    fun seekTo(positionMs: Long)
    fun next()
    /** Back to the start of the track, or to the one before within its first 3 seconds. */
    fun previous()
    /** Straight to the track before, however far into this one it is, as a swipe shows it. */
    fun skipToPrevious()
    /**
     * Plays the best result for [query], as a voice search does; with none, or an empty [query]
     * (as for "play Kithsong"), the first of Recents. Nothing plays if the library has nothing.
     */
    fun playSearch(query: String) {}

    /** Turns shuffle on or off, carrying on from the current track; set it before [play] to start shuffled. */
    fun setShuffle(on: Boolean)

    /** The uids of the queue's tracks, in queue order. */
    val queueUids: List<String>

    /**
     * Stops, keeping the queue and the place in it to play on from, so nothing is left running:
     * no notification, and no foreground service.
     */
    fun stop()

    /** The queue's tracks, in queue order. */
    val queueTracks: List<Track> get() = emptyList()

    /**
     * Swaps the queue for [tracks], the same ones in the same order as the library now has them
     * (a tag edit can move their files). Nothing stops: the track playing plays on from the file
     * it has open.
     */
    fun updateQueue(tracks: List<Track>) {}

    /** Stops and empties the queue, up next too, so nothing is playing: before its tracks are deleted, say. */
    fun clear()

    /** What the app calls the queue: what plays next, ahead of the rest, in the order it plays. */
    val upNext: List<UpNextEntry> get() = emptyList()

    /**
     * Play next or, if not [next], Play last: adds [what], those of its tracks with a file here,
     * as a row each or as one row by the setting. False if it has none.
     */
    suspend fun addUpNext(what: QueueAddition, next: Boolean): Boolean = false

    /** Entry [id] goes to place [to] (0 first). */
    fun moveUpNext(id: Int, to: Int) {}
    fun removeUpNext(id: Int) {}
    /** Empties up next; what's playing goes on. */
    fun clearUpNext() {}

    /** Google Cast: devices to play on, and the one playing now. See ng.kithso.android.cast. */
    val cast: CastControl get() = NoCast
}

/** A Cast device that can be picked: [id] is its MediaRouter route id. */
data class CastDevice(val id: String, val name: String, val description: String?)

/**
 * Playing on a Google Cast device (the Default Media Receiver) instead of the phone. Snapshot
 * state, like [Playback]. Without Google Play Services, [available] is false and the rest does
 * nothing.
 */
@Stable
interface CastControl {
    /** Whether casting can be offered at all: Play Services is on the device and Cast started. */
    val available: Boolean
    /** Devices found on the network while discovery runs ([startDiscovery]). */
    val devices: List<CastDevice>
    /** Whether a cast session is connecting or connected. */
    val connecting: Boolean
    /** Whether a cast session is connected and playback goes to it. */
    val casting: Boolean
    /** The connected (or connecting) device's name, or null. */
    val deviceName: String?
    /** The Cast device's volume, 0 to 1. Only meaningful while [casting]. */
    val volume: Float

    /** Looks for devices, as a picker opens; [stopDiscovery] as it closes. */
    fun startDiscovery()
    fun stopDiscovery()
    /** Starts a cast session on [device]; what's playing moves to it at the same place. */
    fun connect(device: CastDevice)
    /** Ends the cast session, stopping playback on the device. */
    fun disconnect()
    fun setVolume(volume: Float)
}

/** No Cast, as without Play Services, and in previews and tests. */
object NoCast : CastControl {
    override val available get() = false
    override val devices get() = emptyList<CastDevice>()
    override val connecting get() = false
    override val casting get() = false
    override val deviceName: String? get() = null
    override val volume get() = 0f
    override fun startDiscovery() {}
    override fun stopDiscovery() {}
    override fun connect(device: CastDevice) {}
    override fun disconnect() {}
    override fun setVolume(volume: Float) {}
}

/** What Play next and Play last add: track [uid] with no [kind], or else what [uid] is, called [name]. */
data class QueueAddition(val kind: CollectionRef.Kind?, val uid: String, val name: String? = null)

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
    override val hasPrevious: Boolean
        get() = track != null && index > 0
    override val nextTrack: Track? get() = if (hasNext) queue[index + 1] else null
    override val previousTrack: Track? get() = if (hasPrevious) queue[index - 1] else null
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

    override fun play(queue: List<Track>, index: Int, origin: CorePlayer.Origin) {
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

    override fun skipToPrevious() {
        if (hasPrevious) load(queue, index - 1)
    }

    override fun setShuffle(on: Boolean) {
        shuffleOn = on
    }

    override val queueUids: List<String> get() = queue.map { it.uid }

    override fun stop() = playOrPause(false)

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
