package ng.kithso.android.playback

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import androidx.annotation.OptIn
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.SimpleBasePlayer
import androidx.media3.common.Tracks
import androidx.media3.common.audio.AudioBecomingNoisyManager
import androidx.media3.common.audio.AudioFocusManager
import androidx.media3.common.util.Clock
import androidx.media3.common.util.UnstableApi
import ng.kithso.core.CorePlayer
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture

/**
 * musiccore's gapless player as a Media3 [Player], so a media session can drive it: the core
 * decodes and plays, and this keeps the queue of [MediaItem]s (see [toMediaItem]) and what
 * Media3 needs around it. It also does what an Android player must and the core doesn't know
 * about: audio focus, pausing when headphones are unplugged, and a wake lock while playing.
 * Plays are counted to [onPlayed] with the track uid and when the play began (wall clock ms)
 * when the core says they count (half the track or four minutes listened, whichever is less).
 * [onNowPlaying] gets each track's uid as it starts playing, for scrobbling.
 *
 * Shuffle is the core's (mc_player_set_shuffle), offered as Media3's shuffle mode and starting
 * as [shuffle]; each change goes to [onShuffleChanged], to be kept for next time. The core's
 * shuffled order is published as the timeline's shuffle order ([OrderedTimeline]), so
 * controllers know what comes next; next and previous are still passed to the core as such,
 * and next is always offered while shuffled, as the core's shuffle never ends. The core's
 * [volume] (0 to 100) is set to begin with.
 *
 * Use it on the main thread.
 */
@OptIn(UnstableApi::class)
class CoreMediaPlayer(
    context: Context,
    private var shuffle: Boolean,
    volume: Int,
    private val onShuffleChanged: (Boolean) -> Unit,
    private val onNowPlaying: (String) -> Unit = {},
    private val onPlayed: (String, Long) -> Unit,
) : SimpleBasePlayer(Looper.getMainLooper()) {
    private val main = Handler(Looper.getMainLooper())
    private val core = CorePlayer { event -> main.post { onCoreEvent(event) } }.apply {
        setShuffle(shuffle)
        setVolume(volume)
    }

    // No ducking here yet, though the core has a volume now. From Android 8 the system ducks
    // the app itself, since music doesn't ask to be paused instead.
    private val focus = AudioFocusManager(context, Looper.getMainLooper(), object : AudioFocusManager.PlayerControl {
        override fun setVolumeMultiplier(volumeMultiplier: Float) = Unit
        override fun executePlayerCommand(playerCommand: Int) = onFocusCommand(playerCommand)
    }).apply { setAudioAttributes(AUDIO_ATTRIBUTES) }
    private val noisy = AudioBecomingNoisyManager(
        context, Looper.getMainLooper(), Looper.getMainLooper(), { playWhenReady = false }, Clock.DEFAULT,
    )
    private val wakeLock = (context.getSystemService(Context.POWER_SERVICE) as PowerManager)
        .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "kithsong:playback")
        .apply { setReferenceCounted(false) }

    private var items: List<MediaItem> = emptyList()
    // The core's shuffled order for Media3, worked out again when the current item or the
    // order changes: for item [orderIndex], while [orderVersion] is [orderMade].
    private var order: IntArray? = null
    private var orderIndex = -1
    private var orderMade = -1
    private var orderVersion = 0
    // What was asked for. Playing also needs audio focus: while it's lost for a moment
    // (a call, say), [suppressed] holds playback paused.
    private var wantPlaying = false
    private var wantPlayingReason = Player.PLAY_WHEN_READY_CHANGE_REASON_USER_REQUEST
    private var suppressed = false
    // After stop() and before prepare().
    private var idle = true
    // A new queue is loaded into the core stopped, to start at [startIndex] and
    // [startPositionMs] once it's first played; shuffled, the core picks the first item.
    private var started = false
    // The item last passed to [onNowPlaying], -1 while not playing.
    private var announced = -1
    private var startIndex = 0
    private var startPositionMs = 0L

    override fun getState(): State {
        val status = core.status
        val index = if (started) status.index.coerceAtLeast(0) else startIndex
        val position: PositionSupplier =
            if (started) PositionSupplier { core.status.positionMs } else PositionSupplier.getConstant(startPositionMs)
        val commands = Player.Commands.Builder().addAll(
            COMMAND_PLAY_PAUSE, COMMAND_PREPARE, COMMAND_STOP, COMMAND_RELEASE,
            COMMAND_SET_MEDIA_ITEM, COMMAND_CHANGE_MEDIA_ITEMS,
            COMMAND_GET_CURRENT_MEDIA_ITEM, COMMAND_GET_TIMELINE, COMMAND_GET_METADATA, COMMAND_GET_AUDIO_ATTRIBUTES,
            COMMAND_SET_SHUFFLE_MODE,
        )
        if (items.isNotEmpty()) {
            commands.addAll(
                COMMAND_SEEK_IN_CURRENT_MEDIA_ITEM, COMMAND_SEEK_TO_DEFAULT_POSITION, COMMAND_SEEK_TO_MEDIA_ITEM,
                // Previous restarts the track before going back, so it always does something.
                COMMAND_SEEK_TO_PREVIOUS, COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM,
            )
            if (shuffle || index < items.lastIndex) commands.addAll(COMMAND_SEEK_TO_NEXT, COMMAND_SEEK_TO_NEXT_MEDIA_ITEM)
        }
        val state = State.Builder()
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
            .setShuffleModeEnabled(shuffle)
            .build()
        val order = if (shuffle) shuffledOrder(index) else null
        return if (order == null) state
        else state.buildUpon().setPlaylist(OrderedTimeline(state.timeline, order), Tracks.EMPTY, state.currentMetadata).build()
    }

    // The core's shuffled order as a permutation of the queue, as Media3's timeline needs: the
    // items that played before [current], then it and those that play after it, the nearest
    // of each first, each item once. Null if the core is on another item just now.
    private fun shuffledOrder(current: Int): IntArray? {
        val n = items.size
        if (n == 0) return null
        if (current == orderIndex && orderMade == orderVersion) return order
        // The places n - 1 either side hold every item: a round of n places plays each once.
        val around = core.order(-(n - 1), 2 * n - 1)
        val mid = n - 1
        if (around[mid] != current) return null
        val seen = BooleanArray(n).also { it[current] = true }
        val before = ArrayList<Int>()
        val after = ArrayList<Int>()
        for (d in 1 until n) {
            around[mid + d].let { if (it in 0 until n && !seen[it]) after += it.also { seen[it] = true } }
            around[mid - d].let { if (it in 0 until n && !seen[it]) before += it.also { seen[it] = true } }
        }
        for (i in 0 until n) if (!seen[i]) after += i
        val made = (before.asReversed() + current + after).toIntArray()
        order = made
        orderIndex = current
        orderMade = orderVersion
        return made
    }

    override fun handleSetMediaItems(mediaItems: List<MediaItem>, startIndex: Int, startPositionMs: Long): ListenableFuture<*> {
        takePlays() // the old queue's, while [items] still says which tracks they were
        items = mediaItems
        this.startIndex = if (startIndex == C.INDEX_UNSET) 0 else startIndex.coerceIn(0, (items.size - 1).coerceAtLeast(0))
        this.startPositionMs = if (startPositionMs == C.TIME_UNSET) 0 else startPositionMs
        started = false
        // Unset, shuffled, the core picks the first item.
        val start = if (startIndex == C.INDEX_UNSET && shuffle) -1 else this.startIndex
        core.loadQueue(items.map { it.path.orEmpty() }, start)
        if (items.isNotEmpty()) this.startIndex = core.status.index.coerceAtLeast(0)
        orderVersion++
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
        } else if (shuffle && seekCommand in SHUFFLED_SEEKS) {
            // The core's shuffled order, not Media3's idea of the next or previous item.
            val paused = core.status.state != CorePlayer.State.Playing
            if (seekCommand == COMMAND_SEEK_TO_NEXT || seekCommand == COMMAND_SEEK_TO_NEXT_MEDIA_ITEM) core.next() else core.previous()
            if (paused) core.pause()
        } else {
            val status = core.status
            if (mediaItemIndex != C.INDEX_UNSET && mediaItemIndex != status.index) {
                // Skipping plays; keep a paused player paused. Shuffled, the rest are shuffled
                // afresh after it.
                core.skipTo(mediaItemIndex)
                orderVersion++
                if (status.state != CorePlayer.State.Playing) core.pause()
            }
            if (position > 0 || mediaItemIndex == status.index) core.seekMs(position)
        }
        return Futures.immediateVoidFuture()
    }

    override fun handleSetShuffleModeEnabled(shuffleModeEnabled: Boolean): ListenableFuture<*> {
        if (shuffleModeEnabled != shuffle) {
            shuffle = shuffleModeEnabled
            core.setShuffle(shuffleModeEnabled)
            orderVersion++
            onShuffleChanged(shuffleModeEnabled)
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
        if (wakeLock.isHeld) wakeLock.release()
        takePlays()
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
                // Loaded stopped on its item, unless another was picked since.
                if (startIndex == core.status.index) {
                    core.resume()
                } else {
                    core.skipTo(startIndex)
                    orderVersion++
                }
                if (startPositionMs > 0) core.seekMs(startPositionMs)
                started = true
            } else if (core.status.state != CorePlayer.State.Playing) {
                core.resume()
            }
        } else if (started && core.status.state == CorePlayer.State.Playing) {
            core.pause()
        }
        noisy.setEnabled(playing)
        if (playing) {
            wakeLock.acquire()
        } else if (wakeLock.isHeld) {
            wakeLock.release()
        }
    }

    private fun onCoreEvent(event: CorePlayer.Event) {
        val status = core.status
        when (event) {
            CorePlayer.Event.Played -> takePlays()
            CorePlayer.Event.Error -> Log.w(TAG, "The core skipped a track or has no audio device")
            CorePlayer.Event.Track, CorePlayer.Event.State -> Unit
        }
        if (status.state != CorePlayer.State.Playing) {
            announced = -1
        } else if (status.index != announced) {
            announced = status.index
            items.getOrNull(status.index)?.let { onNowPlaying(it.mediaId) }
        }
        // It stopped by itself: the queue played out, or there's no audio device.
        if (started && status.state != CorePlayer.State.Playing && wantPlaying && !suppressed) {
            wantPlaying = false
            wantPlayingReason = PLAY_WHEN_READY_CHANGE_REASON_END_OF_MEDIA_ITEM
            update()
        }
        invalidateState()
    }

    // Passes on the plays the core says count. Its indexes are into [items].
    private fun takePlays() {
        while (true) {
            val played = core.takePlayed() ?: break
            items.getOrNull(played.index)?.let { onPlayed(it.mediaId, played.startedMs) }
        }
    }

    private companion object {
        const val TAG = "kithsong"

        val SHUFFLED_SEEKS = setOf(
            COMMAND_SEEK_TO_NEXT, COMMAND_SEEK_TO_NEXT_MEDIA_ITEM, COMMAND_SEEK_TO_PREVIOUS, COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM,
        )

        val AUDIO_ATTRIBUTES: AudioAttributes = AudioAttributes.Builder()
            .setUsage(C.USAGE_MEDIA)
            .setContentType(C.AUDIO_CONTENT_TYPE_MUSIC)
            .build()
    }
}
