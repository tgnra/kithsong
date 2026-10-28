package ng.kithso.android.playback

import android.content.ComponentName
import android.content.Context
import android.util.Log
import androidx.annotation.OptIn
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.ContextCompat
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.common.util.Util
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import ng.kithso.android.data.Track
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.concurrent.ExecutionException

/**
 * [Playback] through the media session of [PlaybackService], with a MediaController. The
 * controller connects in the background; anything asked for before then is done once it has.
 * Use it on the main thread; [scope] must be on it too.
 */
@OptIn(UnstableApi::class)
class SessionPlayback(context: Context, private val scope: CoroutineScope) : Playback {
    override var track by mutableStateOf<Track?>(null)
        private set
    override var isPlaying by mutableStateOf(false)
        private set
    override var positionMs by mutableLongStateOf(0L)
        private set
    override var hasNext by mutableStateOf(false)
        private set

    private var controller: MediaController? = null
    private val pending = ArrayList<(MediaController) -> Unit>()
    private var ticker: Job? = null

    private val listener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) = update(player)
    }

    init {
        val token = SessionToken(context, ComponentName(context, PlaybackService::class.java))
        val future = MediaController.Builder(context, token).buildAsync()
        future.addListener({
            val controller = try {
                future.get()
            } catch (e: ExecutionException) {
                Log.e(TAG, "Couldn't connect to the media session", e)
                return@addListener
            }
            this.controller = controller
            controller.addListener(listener)
            update(controller)
            pending.forEach { it(controller) }
            pending.clear()
        }, ContextCompat.getMainExecutor(context))
    }

    override fun play(queue: List<Track>, index: Int) {
        if (queue.getOrNull(index)?.path == null) return
        val playable = queue.filter { it.path != null }
        val start = queue.subList(0, index).count { it.path != null }
        track = playable[start]
        positionMs = 0
        withController {
            it.setMediaItems(playable.map(Track::toMediaItem), start, 0)
            it.prepare()
            it.play()
        }
    }

    override fun toggle() = withController { Util.handlePlayPauseButtonAction(it) }

    override fun seekTo(positionMs: Long) {
        this.positionMs = positionMs
        withController { it.seekTo(positionMs) }
    }

    override fun next() = withController { it.seekToNext() }

    override fun previous() = withController { it.seekToPrevious() }

    private fun withController(action: (MediaController) -> Unit) {
        val controller = controller
        if (controller != null) action(controller) else pending += action
    }

    private fun update(player: Player) {
        track = player.currentMediaItem?.toTrack()
        // As the play/pause button should show it, so it doesn't flicker while starting.
        isPlaying = !Util.shouldShowPlayButton(player)
        hasNext = player.hasNextMediaItem()
        positionMs = player.currentPosition
        ticker?.cancel()
        ticker = if (!player.isPlaying) null else scope.launch {
            while (isActive) {
                delay(TICK_MS)
                positionMs = player.currentPosition
            }
        }
    }

    private companion object {
        const val TAG = "kithsong"
        const val TICK_MS = 250L
    }
}
