package app.musicapp.playback

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import androidx.annotation.OptIn
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.SimpleBasePlayer
import androidx.media3.common.audio.AudioBecomingNoisyManager
import androidx.media3.common.audio.AudioFocusManager
import androidx.media3.common.util.Clock
import androidx.media3.common.util.UnstableApi
import app.musicapp.CorePlayer
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture

/**
 * musiccore's gapless player as a Media3 [Player], so a media session can drive it: the core
 * decodes and plays, and this keeps the queue of [MediaItem]s (see [toMediaItem]) and what
 * Media3 needs around it. It also does what an Android player must and the core doesn't know
 * about: audio focus, pausing when headphones are unplugged, and a wake lock while playing.
 * Plays are counted to [onPlayed] with the track uid once a track has been listened to for
 * half its length or four minutes, whichever is less.
 *
 * Use it on the main thread.
 */
@OptIn(UnstableApi::class)
class CoreMediaPlayer(context: Context, private val onPlayed: (String) -> Unit) : SimpleBasePlayer(Looper.getMainLooper()) {
    private val main = Handler(Looper.getMainLooper())
    private val core = CorePlayer { event -> main.post { onCoreEvent(event) } }

    // The core has no volume control, so there's no ducking here. From Android 8 the system
    // ducks the app itself, since music doesn't ask to be paused instead.
    private val focus = AudioFocusManager(context, Looper.getMainLooper(), object : AudioFocusManager.PlayerControl {
        override fun setVolumeMultiplier(volumeMultiplier: Float) = Unit
        override fun executePlayerCommand(playerCommand: Int) = onFocusCommand(playerCommand)
    }).apply { setAudioAttributes(AUDIO_ATTRIBUTES) }
    private val noisy = AudioBecomingNoisyManager(
        context, Looper.getMainLooper(), Looper.getMainLooper(), { playWhenReady = false }, Clock.DEFAULT,
    )
    private val wakeLock = (context.getSystemService(Context.POWER_SERVICE) as PowerManager)
        .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "musicapp:playback")
        .apply { setReferenceCounted(false) }

    private var items: List<MediaItem> = emptyList()
    // What was asked for. Playing also needs audio focus: while it's lost for a moment
    // (a call, say), [suppressed] holds playback paused.
    private var wantPlaying = false
    private var wantPlayingReason = Player.PLAY_WHEN_READY_CHANGE_REASON_USER_REQUEST
    private var suppressed = false
    // After stop() and before prepare().
    private var idle = true
    // Giving the core a queue starts playing it, so a new queue stays here, to start at
    // [startIndex] and [startPositionMs], until it's first played.
    private var started = false
    private var startIndex = 0
    private var startPositionMs = 0L

    // How long the current item has been heard for, to count a play.
    private var listenedMs = 0L
    private var counted = false
    private var lastTick = 0L
    private val tick = object : Runnable {
        override fun run() {
            countPlay()
            main.postDelayed(this, TICK_MS)
        }
    }

    override fun getState(): State {
        val status = core.status
        val index = if (started) status.index.coerceAtLeast(0) else startIndex
        val position: PositionSupplier =
            if (started) PositionSupplier { core.status.positionMs } else PositionSupplier.getConstant(startPositionMs)
        val commands = Player.Commands.Builder().addAll(
            COMMAND_PLAY_PAUSE, COMMAND_PREPARE, COMMAND_STOP, COMMAND_RELEASE,
            COMMAND_SET_MEDIA_ITEM, COMMAND_CHANGE_MEDIA_ITEMS,
            COMMAND_GET_CURRENT_MEDIA_ITEM, COMMAND_GET_TIMELINE, COMMAND_GET_METADATA, COMMAND_GET_AUDIO_ATTRIBUTES,
        )
        if (items.isNotEmpty()) {
            commands.addAll(
                COMMAND_SEEK_IN_CURRENT_MEDIA_ITEM, COMMAND_SEEK_TO_DEFAULT_POSITION, COMMAND_SEEK_TO_MEDIA_ITEM,
                // Previous restarts the track before going back, so it always does something.
                COMMAND_SEEK_TO_PREVIOUS, COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM,
            )
            if (index < items.lastIndex) commands.addAll(COMMAND_SEEK_TO_NEXT, COMMAND_SEEK_TO_NEXT_MEDIA_ITEM)
        }
        return State.Builder()
            .setAvailableCommands(commands.build())
            .setAudioAttributes(AUDIO_ATTRIBUTES)
            .setPlaylist(items.mapIndexed { i, item ->
                // A playlist can hold a track twice, so the index is the uid.
                MediaItemData.Builder(i)
                    .setMediaItem(item)
                    .setDurationUs(item.mediaMetadata.durationMs?.let { it * 1000 } ?: C.TIME_UNSET)
                    .setIsSeekable(true)
                    .build()
            })
            .setCurrentMediaItemIndex(index)
            .setContentPositionMs(position)
            .setPlaybackState(if (idle || items.isEmpty()) STATE_IDLE else STATE_READY)
            .setPlayWhenReady(wantPlaying, wantPlayingReason)
            .setPlaybackSuppressionReason(
                if (suppressed) PLAYBACK_SUPPRESSION_REASON_TRANSIENT_AUDIO_FOCUS_LOSS else PLAYBACK_SUPPRESSION_REASON_NONE,
            )
            // The core restarts the track within its first 3 seconds, and so does Media3.
            .setMaxSeekToPreviousPositionMs(3000)
            .build()
    }

    override fun handleSetMediaItems(mediaItems: List<MediaItem>, startIndex: Int, startPositionMs: Long): ListenableFuture<*> {
        items = mediaItems
        this.startIndex = if (startIndex == C.INDEX_UNSET) 0 else startIndex.coerceIn(0, (items.size - 1).coerceAtLeast(0))
        this.startPositionMs = if (startPositionMs == C.TIME_UNSET) 0 else startPositionMs
        started = false
        core.stop()
        resetCount()
        update()
        return Futures.immediateVoidFuture()
    }

    override fun handleSetPlayWhenReady(playWhenReady: Boolean): ListenableFuture<*> {
        wantPlaying = playWhenReady
        wantPlayingReason = PLAY_WHEN_READY_CHANGE_REASON_USER_REQUEST
        update()
        return Futures.immediateVoidFuture()
    }

    override fun handlePrepare(): ListenableFuture<*> {
        idle = false
        update()
        return Futures.immediateVoidFuture()
    }

    override fun handleStop(): ListenableFuture<*> {
        idle = true
        core.stop()
        update()
        return Futures.immediateVoidFuture()
    }

    override fun handleSeek(mediaItemIndex: Int, positionMs: Long, seekCommand: Int): ListenableFuture<*> {
        val position = if (positionMs == C.TIME_UNSET) 0 else positionMs
        if (!started) {
            if (mediaItemIndex != C.INDEX_UNSET) startIndex = mediaItemIndex
            startPositionMs = position
        } else {
            val status = core.status
            if (mediaItemIndex != C.INDEX_UNSET && mediaItemIndex != status.index) {
                // Skipping plays; keep a paused player paused.
                core.skipTo(mediaItemIndex)
                if (status.state != CorePlayer.State.Playing) core.pause()
                resetCount()
            }
            if (position > 0 || mediaItemIndex == status.index) core.seekMs(position)
            if (position == 0L) resetCount()
        }
        return Futures.immediateVoidFuture()
    }

    // Queue editing isn't offered by the app; controllers that try it change nothing.
    override fun handleAddMediaItems(index: Int, mediaItems: List<MediaItem>): ListenableFuture<*> = Futures.immediateVoidFuture()
    override fun handleRemoveMediaItems(fromIndex: Int, toIndex: Int): ListenableFuture<*> = Futures.immediateVoidFuture()
    override fun handleMoveMediaItems(fromIndex: Int, toIndex: Int, newIndex: Int): ListenableFuture<*> = Futures.immediateVoidFuture()
    override fun handleReplaceMediaItems(fromIndex: Int, toIndex: Int, mediaItems: List<MediaItem>): ListenableFuture<*> =
        Futures.immediateVoidFuture()

    override fun handleRelease(): ListenableFuture<*> {
        focus.release()
        noisy.setEnabled(false)
        main.removeCallbacks(tick)
        if (wakeLock.isHeld) wakeLock.release()
        core.close()
        return Futures.immediateVoidFuture()
    }

    // Asks for audio focus if playing is wanted, then plays or pauses to match.
    private fun update() {
        val wanted = wantPlaying && !idle && items.isNotEmpty()
        val command = focus.updateAudioFocus(wanted, if (wanted) STATE_READY else STATE_IDLE)
        if (wanted) applyFocusCommand(command)
        play(wantPlaying && !suppressed && !idle && items.isNotEmpty())
    }

    private fun onFocusCommand(command: Int) {
        applyFocusCommand(command)
        play(wantPlaying && !suppressed && !idle && items.isNotEmpty())
        invalidateState()
    }

    private fun applyFocusCommand(command: Int) {
        when (command) {
            AudioFocusManager.PLAYER_COMMAND_DO_NOT_PLAY -> {
                wantPlaying = false
                wantPlayingReason = PLAY_WHEN_READY_CHANGE_REASON_AUDIO_FOCUS_LOSS
                suppressed = false
            }
            AudioFocusManager.PLAYER_COMMAND_WAIT_FOR_CALLBACK -> suppressed = true
            AudioFocusManager.PLAYER_COMMAND_PLAY_WHEN_READY -> suppressed = false
        }
    }

    private fun play(playing: Boolean) {
        if (playing) {
            if (!started) {
                core.setQueue(items.map { it.path.orEmpty() }, startIndex)
                if (startPositionMs > 0) core.seekMs(startPositionMs)
                started = true
            } else if (core.status.state != CorePlayer.State.Playing) {
                core.resume()
            }
        } else if (started && core.status.state == CorePlayer.State.Playing) {
            core.pause()
        }
        noisy.setEnabled(playing)
        main.removeCallbacks(tick)
        if (playing) {
            wakeLock.acquire()
            lastTick = SystemClock.elapsedRealtime()
            main.postDelayed(tick, TICK_MS)
        } else if (wakeLock.isHeld) {
            wakeLock.release()
        }
    }

    private fun onCoreEvent(event: CorePlayer.Event) {
        val status = core.status
        when (event) {
            CorePlayer.Event.Track -> resetCount()
            CorePlayer.Event.Error -> Log.w(TAG, "The core skipped a track or has no audio device")
            CorePlayer.Event.State -> Unit
        }
        // It stopped by itself: the queue played out, or there's no audio device.
        if (started && status.state != CorePlayer.State.Playing && wantPlaying && !suppressed) {
            wantPlaying = false
            wantPlayingReason = PLAY_WHEN_READY_CHANGE_REASON_END_OF_MEDIA_ITEM
            resetCount()
            update()
        }
        invalidateState()
    }

    private fun resetCount() {
        listenedMs = 0
        counted = false
    }

    private fun countPlay() {
        val now = SystemClock.elapsedRealtime()
        val elapsed = now - lastTick
        lastTick = now
        val status = core.status
        val item = items.getOrNull(status.index) ?: return
        if (counted || status.state != CorePlayer.State.Playing) return
        listenedMs += elapsed
        val durationMs = status.durationMs.takeIf { it > 0 } ?: item.mediaMetadata.durationMs ?: 0
        val needed = if (durationMs > 0) minOf(durationMs / 2, MAX_PLAY_MS) else MAX_PLAY_MS
        if (listenedMs >= needed) {
            counted = true
            onPlayed(item.mediaId)
        }
    }

    private companion object {
        const val TAG = "musicapp"
        const val TICK_MS = 1000L
        const val MAX_PLAY_MS = 4 * 60 * 1000L
        val AUDIO_ATTRIBUTES: AudioAttributes = AudioAttributes.Builder()
            .setUsage(C.USAGE_MEDIA)
            .setContentType(C.AUDIO_CONTENT_TYPE_MUSIC)
            .build()
    }
}
