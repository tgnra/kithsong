package app.kithsong.wear

import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import app.kithsong.AlbumInfo
import app.kithsong.CorePlayer
import app.kithsong.TrackInfo

/** musiccore's player, with what it is playing as Compose state. Use on the main thread. */
class Player {
    private val main = Handler(Looper.getMainLooper())
    private val core = CorePlayer { main.post(::refresh) }

    var album by mutableStateOf<AlbumInfo?>(null)
        private set
    /** The queue: the album's tracks that have a file on the watch. */
    var queue by mutableStateOf<List<TrackInfo>>(emptyList())
        private set
    var index by mutableIntStateOf(-1)
        private set
    var isPlaying by mutableStateOf(false)
        private set

    val track: TrackInfo? get() = queue.getOrNull(index)
    // Previous restarts the track before going back, so it always does something.
    val hasPrevious: Boolean get() = track != null
    val hasNext: Boolean get() = index in 0 until queue.lastIndex

    /** Plays [album] from [track]; the rest of the album is the queue. */
    fun play(album: AlbumInfo, tracks: List<TrackInfo>, track: TrackInfo) {
        val playable = tracks.filter { it.path != null }
        val start = playable.indexOf(track)
        if (start < 0) return
        this.album = album
        queue = playable
        core.setQueue(playable.map { it.path!! }, start)
        refresh()
    }

    // The core sends events only for changes it makes itself (a track ending, an error), so
    // refresh after each command.
    fun togglePlayPause() {
        core.toggle()
        refresh()
    }

    fun skipPrevious() {
        core.previous()
        refresh()
    }

    fun skipNext() {
        if (!hasNext) return
        core.next()
        refresh()
    }

    private fun refresh() {
        val status = core.status
        index = status.index
        isPlaying = status.state == CorePlayer.State.Playing
    }
}
