package ng.kithso.playback

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
 * kithcore's gapless player as a Media3 [Player], so a media session can drive it: the core
 * decodes and plays, and this keeps the queue of [MediaItem]s (each with its file, see [path])
 * and what Media3 needs around it. It also does what an Android player must and the core
 * doesn't know about: audio focus, pausing when headphones are unplugged, and a wake lock while
 * playing.
 * Plays are counted to [onPlayed] with the track uid and when the play began (wall clock ms)
 * when the core says they count (half the track or four minutes listened, whichever is less).
 * [onNowPlaying] gets each track's uid as it starts playing, for scrobbling. [onPlayEnded] gets
 * every play as it ends, counted or not, for the play log; a queue's items say what it is
 * ([playOrigin]).
 *
 * Shuffle is the core's (mc_player_set_shuffle), offered as Media3's shuffle mode and starting
 * as [shuffle]; each change goes to [onShuffleChanged], to be kept for next time. The core's
 * shuffled order is published as the timeline's shuffle order ([OrderedTimeline]), so
 * controllers know what comes next; next and previous are still passed to the core as such,
 * and next is always offered while shuffled, as the core's shuffle never ends. The core's
 * [volume] (0 to 100) is set to begin with. Ducking for another app's sound lowers the core's
 * volume for a while, then puts [volume] back.
 *
 * A queue of one item whose [playOrigin] is the radio is a station from that track, in the
 * library in [libraryDir], which the core then keeps adding to (mc_player_play_station), and the
 * origin says what the station is, which needn't be that track. Media3
 * wants a queue up front, so [items] follows the core's: at each event, whichever it is as the
 * queue event can lag by seconds behind the core's count, what's new goes to [stationItems] to be
 * looked up, and the items it gives back are added to the end. Until then the core can be ahead
 * of [items], so the current item Media3 is told of stops at the last one known. A station plays
 * in its own order, so meanwhile it's shown as not shuffled, with [shuffle] kept for the next
 * queue, and next is offered and passed to the core as such, as it has one when Media3 sees none.
 *
 * Use it on the main thread.
 */
@OptIn(UnstableApi::class)
class CoreMediaPlayer(
    context: Context,
    private var shuffle: Boolean,
    private val volume: Int,
    private val onShuffleChanged: (Boolean) -> Unit,
    private val onNowPlaying: (String) -> Unit = {},
    private val onPlayEnded: (CorePlayer.PlayEvent) -> Unit = {},
    private val libraryDir: String? = null,
    /**
     * Gives the done callback, on any thread, one queue item for each of a station's track uids,
     * in order: one for a track the library no longer has too, as indexes have to stay in step
     * with the core's. All are of a queue that is the origin.
     */
    private val stationItems: (List<String>, CorePlayer.Origin, (List<MediaItem>) -> Unit) -> Unit = { _, _, _ -> },
    private val onPlayed: (String, Long) -> Unit,
) : SimpleBasePlayer(Looper.getMainLooper()) {
    private val main = Handler(Looper.getMainLooper())
    private val core = CorePlayer { event -> main.post { onCoreEvent(event) } }.apply {
        setShuffle(shuffle)
        setVolume(volume)
    }

    // From Android 8 the system ducks the app itself, without telling it, since music doesn't
    // ask to be paused instead; before that the focus manager asks for it here.
    private val focus = AudioFocusManager(context, Looper.getMainLooper(), object : AudioFocusManager.PlayerControl {
        override fun setVolumeMultiplier(volumeMultiplier: Float) = duck(volumeMultiplier)
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
    // The queue is a station's, even once the station has stopped, as the core keeps its order.
    private var station = false
    // Loaded, and yet to be started as a station, which plays at once.
    // Goes for a station that hasn't played yet: how many more times to get it past a failure.
    private var stationTries = 0
    private var stationWaiting = false
    // While new items are being looked up. [queueVersion] tells whether they're still wanted.
    private var growing = false
    private var queueVersion = 0

    override fun getState(): State {
        val status = core.status
        // The core may be on an item that's still being looked up.
        val index = if (started) status.index.coerceAtMost(items.lastIndex).coerceAtLeast(0) else startIndex
        val shuffled = shuffle && !station
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
            val more = status.radio || stationWaiting || index < maxOf(items.size, status.count) - 1
            if (shuffled || more) commands.addAll(COMMAND_SEEK_TO_NEXT, COMMAND_SEEK_TO_NEXT_MEDIA_ITEM)
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
            .setShuffleModeEnabled(shuffled)
            .build()
        val order = if (shuffled) shuffledOrder(index) else null
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
        queueVersion++
        station = libraryDir != null && items.singleOrNull()?.playOrigin?.context == CorePlayer.PlayContext.Radio
        stationWaiting = station
        stationTries = 0
        // Unset, shuffled, the core picks the first item.
        val start = if (startIndex == C.INDEX_UNSET && shuffle) -1 else this.startIndex
        val origin = items.firstOrNull()?.playOrigin ?: CorePlayer.Origin()
        core.loadQueue(items.map { it.path.orEmpty() }, items.map { it.mediaId }, origin, start)
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
        // Played again, it carries on from here, as with a new queue's starting point.
        if (started) {
            val status = core.status
            startIndex = status.index.coerceAtLeast(0)
            startPositionMs = status.positionMs
            started = false
        }
        core.stop()
        update()
        return Futures.immediateVoidFuture()
    }

    override fun handleSeek(mediaItemIndex: Int, positionMs: Long, seekCommand: Int): ListenableFuture<*> {
        val position = if (positionMs == C.TIME_UNSET) 0 else positionMs
        if (!started) {
            if (mediaItemIndex != C.INDEX_UNSET) startIndex = mediaItemIndex
            startPositionMs = position
        } else if ((shuffle || station) && seekCommand in SHUFFLED_SEEKS) {
            // The core's order, not Media3's idea of the next or previous item: a station's next
            // may be one Media3 hasn't heard of, or one that's yet to be picked.
            val paused = core.status.state != CorePlayer.State.Playing
            // The core's previous restarts a track that's past its first seconds, which is for
            // the previous button; this asks for the item before, whatever the position.
            if (seekCommand == COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM) core.seekMs(0)
            if (seekCommand == COMMAND_SEEK_TO_NEXT || seekCommand == COMMAND_SEEK_TO_NEXT_MEDIA_ITEM) core.next() else core.previous()
            if (paused) core.pause()
            growQueue()
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

    // The same tracks as they are now: what's shown of them, and where their files are, which is
    // all the core knows of them, so it plays on. A queue's other changes go through
    // handleSetMediaItems().
    override fun handleReplaceMediaItems(fromIndex: Int, toIndex: Int, mediaItems: List<MediaItem>): ListenableFuture<*> {
        if (toIndex - fromIndex == mediaItems.size) {
            mediaItems.forEachIndexed { i, item ->
                val path = item.path
                if (path != null && path != items[fromIndex + i].path) core.setQueuePath(fromIndex + i, path)
            }
            items = items.take(fromIndex) + mediaItems + items.drop(toIndex)
            update()
        }
        return Futures.immediateVoidFuture()
    }

    override fun handleRelease(): ListenableFuture<*> {
        focus.release()
        noisy.setEnabled(false)
        if (wakeLock.isHeld) wakeLock.release()
        core.endPlay()
        takePlays()
        core.close()
        main.removeCallbacksAndMessages(null) // events and looked up items, for a core that's gone
        return Futures.immediateVoidFuture()
    }

    // Scales the output by [multiplier] (1 when not ducked) from [volume], which stays as the
    // user left it. The core's volume is the cube root of its gain, so the multiplier's cube
    // root scales it.
    private fun duck(multiplier: Float) {
        core.setVolume(Math.round(volume * Math.cbrt(multiplier.toDouble())).toInt())
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
                if (stationWaiting) {
                    stationWaiting = false
                    stationTries = STATION_TRIES
                    val start = items.first()
                    val seed = start.playOrigin.contextUid ?: start.mediaId
                    core.playRadio(libraryDir.orEmpty(), start.path.orEmpty(), start.mediaId, seed)
                } else if (startIndex == core.status.index) {
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
            CorePlayer.Event.Played, CorePlayer.Event.Ended -> takePlays()
            CorePlayer.Event.Error -> {
                Log.w(TAG, "The core skipped a track or has no audio device")
                // A seed that won't open leaves the core stopped with nothing to fall through
                // to yet; next() has it start on what the station adds. Without an audio device
                // that fails too, however often, so it isn't tried for ever.
                if (stationTries > 0 && status.state == CorePlayer.State.Stopped && status.radio) {
                    stationTries--
                    core.next()
                }
            }
            CorePlayer.Event.Track, CorePlayer.Event.State, CorePlayer.Event.Queue -> Unit
        }
        if (status.state == CorePlayer.State.Playing) stationTries = 0
        growQueue()
        if (status.state != CorePlayer.State.Playing) {
            announced = -1
        } else if (status.index != announced) {
            announced = status.index
            uidAt(status.index)?.let(onNowPlaying)
        }
        // Next while paused on a station's last item: what it plays is only added now.
        if (station && started && status.state == CorePlayer.State.Playing && (!wantPlaying || suppressed)) core.pause()
        // It stopped by itself: the queue played out, or there's no audio device. Not while a
        // station is still getting going, as it carries on past a seed that wouldn't open.
        val starting = stationTries > 0 && status.radio
        if (started && status.state != CorePlayer.State.Playing && !starting && wantPlaying && !suppressed) {
            wantPlaying = false
            wantPlayingReason = PLAY_WHEN_READY_CHANGE_REASON_END_OF_MEDIA_ITEM
            update()
        }
        invalidateState()
    }

    // Has what a station added since looked up, one lookup at a time, and goes on until [items]
    // has caught up with the core, which only ever adds to the end.
    private fun growQueue() {
        val from = items.size
        val count = core.status.count
        if (growing || !station || count <= from) return
        val uids = (from until count).map { core.queueUid(it).orEmpty() }
        val version = queueVersion
        growing = true
        stationItems(uids, items.first().playOrigin) { found ->
            main.post {
                growing = false
                if (version == queueVersion && items.size == from && found.size == uids.size) {
                    items = items + found
                    invalidateState()
                }
                growQueue()
            }
        }
    }

    // The uid of queue item [index], which may be one still being looked up.
    private fun uidAt(index: Int): String? = items.getOrNull(index)?.mediaId ?: core.queueUid(index)

    // Passes on the plays the core says count, whose indexes are into the core's queue, and those
    // that ended, which say for themselves what they were.
    private fun takePlays() {
        while (true) {
            val played = core.takePlayed() ?: break
            uidAt(played.index)?.let { onPlayed(it, played.startedMs) }
        }
        while (true) onPlayEnded(core.takeEnded() ?: break)
    }

    private companion object {
        const val TAG = "kithsong"

        // As the core's RADIO_MAX_FAILS, which goes by items that won't open.
        const val STATION_TRIES = 8

        val SHUFFLED_SEEKS = setOf(
            COMMAND_SEEK_TO_NEXT, COMMAND_SEEK_TO_NEXT_MEDIA_ITEM, COMMAND_SEEK_TO_PREVIOUS, COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM,
        )

        val AUDIO_ATTRIBUTES: AudioAttributes = AudioAttributes.Builder()
            .setUsage(C.USAGE_MEDIA)
            .setContentType(C.AUDIO_CONTENT_TYPE_MUSIC)
            .build()
    }
}
