package ng.kithso.playback

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import androidx.annotation.OptIn
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.DeviceInfo
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.SimpleBasePlayer
import androidx.media3.common.Tracks
import androidx.media3.common.audio.AudioBecomingNoisyManager
import androidx.media3.common.audio.AudioFocusManager
import androidx.media3.common.util.Clock
import androidx.media3.common.util.UnstableApi
import ng.kithso.core.CoreLibrary
import ng.kithso.core.CorePlayer
import ng.kithso.core.KithCoreException
import ng.kithso.core.UpNextEntry
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import kotlin.math.roundToInt

/**
 * Where a list of [listSize] items goes on from: item [current] if it's one of them, else [next],
 * the list's next, or with none (-1), [before], the one it had got to.
 */
fun listPlaceOf(current: Int, listSize: Int, next: Int, before: Int): Int = when {
    current < listSize -> current
    next in 0 until listSize -> next
    else -> before.coerceIn(0, (listSize - 1).coerceAtLeast(0))
}

/**
 * kithcore's gapless player as a Media3 [Player], so a media session can drive it: the core
 * decodes and plays, and this keeps the queue of [MediaItem]s (each with its file, see [path]).
 * It also does what an Android player must and the core doesn't: audio focus, pausing when
 * headphones are unplugged, and a wake lock while playing.
 *
 * Plays go to [onPlayed] (track uid, start in wall clock ms) when the core says they count
 * (half the track or four minutes, whichever is less);
 * [onNowPlaying] gets each track as it starts, for scrobbling; [onPlayEnded] gets every play as
 * it ends, counted or not, for the play log.
 *
 * Shuffle is the core's (mc_player_set_shuffle). Its order is published as the timeline's
 * shuffle order ([OrderedTimeline]) so controllers know what comes next, but next and previous
 * still go to the core, and next is always offered while shuffled, as the core's shuffle never
 * ends. Ducking lowers the core's volume for a while, then puts [volume] (0 to 100) back.
 *
 * Up next is the core's too. Media3 wants a queue up front, so [items] follows the core's: new
 * uids go to [addedItems] to be looked up and the results are appended. The core can be ahead
 * of [items] meanwhile (its queue event can lag by seconds), so the current index Media3 sees
 * stops at the last one known. Up next's items sit at the end of [items] in the order added,
 * not played, and stay after they've played: indexes must match the core's. So Media3 can't
 * tell what's next: next and previous go to the core, [itemAt] says what plays either side,
 * and the shuffled timeline order has them where they play. [listItems] and [listPlace] are
 * the queue without up next.
 *
 * While the sound goes to a cast device ([remoteOutput]), the core plays on muted and its
 * output is tapped ([readTap]) for the device to stream; the player then reports remote
 * playback, so the volume keys and output switcher set the cast device's volume.
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
    /**
     * Gives the done callback, on any thread, one queue item for each of the track uids up next
     * added, in order: one for a track the library no longer has too, as indexes have
     * to stay in step with the core's. All are of a queue that is the origin.
     */
    private val addedItems: (List<String>, CorePlayer.Origin, (List<MediaItem>) -> Unit) -> Unit = { _, _, _ -> },
    /** Up next changed: read [upNext] again. */
    private val onUpNextChanged: () -> Unit = {},
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
    // How many of [items] are the queue that was set; what comes after was added to it.
    private var listSize = 0
    private var upNextChanges = 0L
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
    // While new items are being looked up. [queueVersion] tells whether they're still wanted.
    private var growing = false
    private var queueVersion = 0

    /**
     * Up next now, in the order it plays. The rest of the album's row ([UpNextEntry.rest]) has the
     * album's name as its title, and its tracks' length.
     */
    val upNext: List<UpNextEntry>
        get() = core.upNext.map { entry ->
            if (!entry.rest) return@map entry
            val rest = (entry.firstItem until entry.firstItem + entry.tracks).mapNotNull { items.getOrNull(it)?.mediaMetadata }
            entry.copy(
                title = rest.firstOrNull()?.albumTitle?.toString().orEmpty(),
                durationMs = rest.sumOf { it.durationMs ?: 0L },
            )
        }

    /** Another device the sound goes to, whose volume is its own; see [remoteOutput]. */
    interface RemoteOutput {
        /** 0 to 1, as the device last said. */
        val volume: Float
        val muted: Boolean
        fun setVolume(volume: Float)
        fun setMuted(muted: Boolean)
    }

    /**
     * The device the sound goes to instead of the phone, or null for the phone (see the class's
     * comment). Set, the core is muted and its output tapped, for [readTap]; unset, the tap
     * stops and the phone is heard again, a moment later, so a pause asked for just before has
     * taken hold first. Tell [remoteVolumeChanged] when its volume changes.
     */
    var remoteOutput: RemoteOutput? = null
        set(value) {
            if (value === field) return
            val remote = value != null
            if (remote != (field != null)) {
                main.removeCallbacks(unmute)
                if (remote) {
                    core.startTap()
                    core.setMuted(true)
                } else {
                    core.stopTap()
                    main.postDelayed(unmute, UNMUTE_MS)
                }
            }
            field = value
            // Headphones coming out shouldn't pause what plays somewhere else.
            noisy.setEnabled(!remote && wantPlaying && !suppressed && !idle && items.isNotEmpty())
            invalidateState()
        }

    private val unmute = Runnable { if (remoteOutput == null) core.setMuted(false) }

    /** [remoteOutput]'s volume or mute changed. */
    fun remoteVolumeChanged() = invalidateState()

    /**
     * What the core played since the last call, while [remoteOutput] is set: see
     * [CorePlayer.readTap]. From any thread, but not once the player is released.
     */
    fun readTap(out: ShortArray, rate: IntArray): Int = core.readTap(out, rate)

    /**
     * Next by the core's order. Media3's seekToNext goes by the timeline, which doesn't always
     * have what the core plays next: unshuffled, up next's last-added track is the timeline's last
     * item, with nothing after it, and may not be looked up yet. Where the
     * timeline has a next item, it's the same as seekToNextMediaItem; nothing happens if there's
     * no next at all.
     */
    fun skipToNext() {
        if (!isCommandAvailable(COMMAND_SEEK_TO_NEXT_MEDIA_ITEM)) return
        if (hasNextMediaItem() || !(started || core.status.upNext > 0)) {
            seekToNextMediaItem()
            return
        }
        // As handleSeek does for the core's order.
        val paused = core.status.state != CorePlayer.State.Playing
        started = true
        core.next()
        if (paused) core.pause()
        growQueue()
        invalidateState()
    }

    /** Ends the play under way there and then, as stopped, and passes it on to [onPlayEnded]. */
    fun endPlay() {
        core.endPlay()
        takePlays()
    }

    /** See [CorePlayer.addUpNext]. Off the main thread, on [library]'s. */
    fun addUpNext(library: CoreLibrary, kind: CorePlayer.UpNextKind, uid: String, name: String?, asTracks: Boolean) =
        core.addUpNext(library, kind, uid, name, asTracks)

    fun moveUpNext(id: Int, to: Int) = core.moveUpNext(id, to)
    fun removeUpNext(id: Int) = core.removeUpNext(id)
    fun clearUpNext() = core.clearUpNext()

    /**
     * The item [place] places from the current one as they'll play (1 the next, -1 the one
     * before), which Media3's timeline can't tell with up next about; null for none, or one
     * that's still being looked up.
     */
    fun itemAt(place: Int): MediaItem? = items.getOrNull(indexAt(place))

    /** The index in the queue of [itemAt]'s item, or -1. */
    fun indexAt(place: Int): Int {
        val index = core.status.index
        // A seek before the first play moved the start, which the core hears of on playing.
        if (!started && index >= 0 && index != startIndex) return (startIndex + place).takeIf { it in items.indices } ?: -1
        // With nothing current, what's shown as current is up next's first track.
        return core.order(if (index < 0 && place > 0) place + 1 else place, 1)[0]
    }

    /** The queue that was set, without what's been added to it. */
    val listItems: List<MediaItem> get() = items.take(listSize)

    /**
     * Where in [listItems] to go on from, after the core is on item [current]: that item, or on a
     * track of up next, the list's next, or with none, the one it followed. Entries that wait for
     * the list come after its next, so they don't count.
     */
    fun listPlace(current: Int): Int {
        if (current < listSize) return current
        val left = core.upNext.filterNot { it.afterList || it.rest }.sumOf { it.tracks }
        return listPlaceOf(current, listSize, next = core.order(left + 1, 1)[0], before = core.order(-1, 1)[0])
    }

    override fun getState(): State {
        val status = core.status
        // Up next, added with nothing to play, emptied again without having played: its items
        // are only left over, and with nothing else there's nothing to show.
        val shown = if (listSize == 0 && status.index < 0 && status.upNext == 0) emptyList() else items
        val last = shown.lastIndex.coerceAtLeast(0)
        val index = when {
            // The core may be on an item that's still being looked up.
            started -> status.index.coerceAtMost(items.lastIndex).coerceAtLeast(0)
            // Nothing but up next, whose first track is what Play starts.
            status.index < 0 -> core.order(1, 1)[0].coerceIn(0, last)
            else -> startIndex
        }
        val shuffled = shuffle
        val position: PositionSupplier =
            if (started) PositionSupplier { core.status.positionMs } else PositionSupplier.getConstant(startPositionMs)
        val commands = Player.Commands.Builder().addAll(
            COMMAND_PLAY_PAUSE, COMMAND_PREPARE, COMMAND_STOP, COMMAND_RELEASE,
            COMMAND_SET_MEDIA_ITEM, COMMAND_CHANGE_MEDIA_ITEMS,
            COMMAND_GET_CURRENT_MEDIA_ITEM, COMMAND_GET_TIMELINE, COMMAND_GET_METADATA, COMMAND_GET_AUDIO_ATTRIBUTES,
            COMMAND_SET_SHUFFLE_MODE,
        )
        if (shown.isNotEmpty()) {
            commands.addAll(
                COMMAND_SEEK_IN_CURRENT_MEDIA_ITEM, COMMAND_SEEK_TO_DEFAULT_POSITION, COMMAND_SEEK_TO_MEDIA_ITEM,
                // Previous restarts the track before going back, so it always does something.
                COMMAND_SEEK_TO_PREVIOUS, COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM,
            )
            // By the core's order once it's playing: what was added isn't what's next by index.
            val more = status.upNext > 0 ||
                if (started) core.order(1, 1)[0] >= 0 else index < listSize - 1
            if (shuffled || more) commands.addAll(COMMAND_SEEK_TO_NEXT, COMMAND_SEEK_TO_NEXT_MEDIA_ITEM)
        }
        val state = State.Builder()
            .setAvailableCommands(commands.build())
            .setAudioAttributes(AUDIO_ATTRIBUTES)
            .setPlaylist(shown.mapIndexed { i, item ->
                // A playlist can hold a track twice, so the index is the uid.
                MediaItemData.Builder(i)
                    .setMediaItem(item)
                    .setDurationUs(item.mediaMetadata.durationMs?.let { it * 1000 } ?: C.TIME_UNSET)
                    .setIsSeekable(true)
                    .build()
            })
            .setCurrentMediaItemIndex(index)
            .setContentPositionMs(position)
            .setPlaybackState(if (idle || shown.isEmpty()) STATE_IDLE else STATE_READY)
            .setPlayWhenReady(wantPlaying, wantPlayingReason)
            .setPlaybackSuppressionReason(
                if (suppressed) PLAYBACK_SUPPRESSION_REASON_TRANSIENT_AUDIO_FOCUS_LOSS else PLAYBACK_SUPPRESSION_REASON_NONE,
            )
            // The core restarts the track within its first 3 seconds, and so does Media3.
            .setMaxSeekToPreviousPositionMs(3000)
            .setShuffleModeEnabled(shuffled)
            .build()
        val order = if (shuffled && shown.isNotEmpty()) shuffledOrder(index) else null
        val remote = remoteOutput
        if (order == null && remote == null) return state
        val built = state.buildUpon()
        if (order != null) built.setPlaylist(OrderedTimeline(state.timeline, order), Tracks.EMPTY, state.currentMetadata)
        if (remote != null) {
            built.setDeviceInfo(DeviceInfo.Builder(DeviceInfo.PLAYBACK_TYPE_REMOTE).setMinVolume(0).setMaxVolume(VOLUME_STEPS).build())
                .setDeviceVolume((remote.volume * VOLUME_STEPS).roundToInt().coerceIn(0, VOLUME_STEPS))
                .setIsDeviceMuted(remote.muted)
                .setAvailableCommands(
                    state.availableCommands.buildUpon().addAll(
                        COMMAND_GET_DEVICE_VOLUME, COMMAND_SET_DEVICE_VOLUME_WITH_FLAGS, COMMAND_ADJUST_DEVICE_VOLUME_WITH_FLAGS,
                    ).build(),
                )
        }
        return built.build()
    }

    // The remote device's volume, in the steps the volume keys go by.
    override fun handleSetDeviceVolume(deviceVolume: Int, flags: Int): ListenableFuture<*> {
        remoteOutput?.setVolume(deviceVolume.coerceIn(0, VOLUME_STEPS).toFloat() / VOLUME_STEPS)
        return Futures.immediateVoidFuture()
    }

    override fun handleIncreaseDeviceVolume(flags: Int): ListenableFuture<*> = stepVolume(1)
    override fun handleDecreaseDeviceVolume(flags: Int): ListenableFuture<*> = stepVolume(-1)

    override fun handleSetDeviceMuted(muted: Boolean, flags: Int): ListenableFuture<*> {
        remoteOutput?.setMuted(muted)
        return Futures.immediateVoidFuture()
    }

    private fun stepVolume(by: Int): ListenableFuture<*> {
        remoteOutput?.let { handleSetDeviceVolume((it.volume * VOLUME_STEPS).roundToInt() + by, 0) }
        return Futures.immediateVoidFuture()
    }

    // The core's shuffled order as a permutation of the queue, as Media3's timeline needs: the
    // items that played before [current], then it and those that play after it, the nearest
    // of each first, each item once. Null if the core is on another item just now.
    private fun shuffledOrder(current: Int): IntArray? {
        val n = items.size
        if (n == 0) return null
        if (current == orderIndex && orderMade == orderVersion && order?.size == n) return order
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
        listSize = items.size
        this.startIndex = if (startIndex == C.INDEX_UNSET) 0 else startIndex.coerceIn(0, (items.size - 1).coerceAtLeast(0))
        this.startPositionMs = if (startPositionMs == C.TIME_UNSET) 0 else startPositionMs
        started = false
        queueVersion++
        // Unset, shuffled, the core picks the first item.
        val start = if (startIndex == C.INDEX_UNSET && shuffle) -1 else this.startIndex
        val origin = items.firstOrNull()?.playOrigin ?: CorePlayer.Origin()
        // Anything but what's coming back is someone playing it, which clears up next in the core.
        val load = if (items.firstOrNull()?.restored == true) core::restoreQueue else core::loadQueue
        load(items.map { it.path.orEmpty() }, items.map { it.mediaId }, origin, start)
        if (items.isNotEmpty()) this.startIndex = core.status.index.coerceAtLeast(0)
        orderVersion++
        growQueue() // whatever's up next comes along, with new indexes after the list
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
        val toUpNext = seekCommand in NEXT_SEEKS && core.status.upNext > 0
        if (!started && !toUpNext) {
            if (mediaItemIndex != C.INDEX_UNSET) startIndex = mediaItemIndex
            startPositionMs = position
        } else if ((shuffle || items.size > listSize) && seekCommand in CORE_SEEKS) {
            // The core's order, not Media3's idea of the next or previous item: up next's tracks
            // don't play in the order of their indexes.
            val paused = core.status.state != CorePlayer.State.Playing
            started = true
            // The core's previous restarts a track that's past its first seconds, which is for
            // the previous button; this asks for the item before, whatever the position.
            if (seekCommand == COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM) core.seekMs(0)
            if (seekCommand in NEXT_SEEKS) core.next() else core.previous()
            if (paused) core.pause()
            growQueue()
        } else {
            val status = core.status
            if (mediaItemIndex != C.INDEX_UNSET && mediaItemIndex != status.index) {
                // Skipping plays; keep a paused player paused. Shuffled, the rest are shuffled
                // afresh after it.
                if (skipTo(mediaItemIndex) && status.state != CorePlayer.State.Playing) core.pause()
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

    // Whether the core went to item [index]. A track of up next, as a controller can pick from
    // the queue it's shown, isn't in the list to skip to, and plays when its turn comes.
    private fun skipTo(index: Int): Boolean {
        try {
            core.skipTo(index)
        } catch (e: KithCoreException) {
            return false
        }
        orderVersion++
        return true
    }

    // Queue editing through Media3 isn't offered; controllers that try it change nothing. Up next
    // has calls of its own.
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
            renameUpNext()
            update()
        }
        return Futures.immediateVoidFuture()
    }

    // Rows of up next keep up with what an edit changed: a track's, and an album's title. A
    // playlist or group goes by what it was called when it was added.
    private fun renameUpNext() {
        for (entry in core.upNext) {
            if (entry.rest) continue
            val metadata = items.getOrNull(entry.firstItem)?.mediaMetadata ?: continue
            val album = entry.context == CorePlayer.PlayContext.Album.ordinal
            val title = if (entry.block) metadata.albumTitle.takeIf { album } else metadata.title
            val artist = if (entry.block) entry.artist else metadata.artist?.toString()
            if (title != null && (title.toString() != entry.title || artist != entry.artist)) {
                core.renameUpNext(entry.id, title.toString(), artist)
            }
        }
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
                start()
            } else if (core.status.state != CorePlayer.State.Playing) {
                core.resume()
            }
        } else if (started && core.status.state == CorePlayer.State.Playing) {
            core.pause()
        }
        noisy.setEnabled(playing && remoteOutput == null)
        if (playing) {
            wakeLock.acquire()
        } else if (wakeLock.isHeld) {
            wakeLock.release()
        }
    }

    // Starts a queue loaded stopped, playing.
    private fun start() {
        // Loaded stopped on its item, unless another was picked since.
        if (startIndex == core.status.index || core.status.index < 0 || !skipTo(startIndex)) {
            core.resume() // with nothing current, up next's first track
        }
        if (startPositionMs > 0) core.seekMs(startPositionMs)
        started = true
    }

    private fun onCoreEvent(event: CorePlayer.Event) {
        val status = core.status
        when (event) {
            CorePlayer.Event.Played, CorePlayer.Event.Ended -> takePlays()
            CorePlayer.Event.Error -> Log.w(TAG, "The core skipped a track or has no audio device")
            CorePlayer.Event.Track, CorePlayer.Event.State, CorePlayer.Event.Queue, CorePlayer.Event.UpNext -> Unit
        }
        if (status.upNextChanges != upNextChanges) {
            upNextChanges = status.upNextChanges
            orderVersion++
            onUpNextChanged()
        }
        growQueue()
        if (status.state != CorePlayer.State.Playing) {
            announced = -1
        } else if (status.index != announced) {
            announced = status.index
            uidAt(status.index)?.let(onNowPlaying)
        }
        // It stopped by itself: the queue played out, or there's no audio device.
        if (started && status.state != CorePlayer.State.Playing && wantPlaying && !suppressed) {
            wantPlaying = false
            wantPlayingReason = PLAY_WHEN_READY_CHANGE_REASON_END_OF_MEDIA_ITEM
            update()
        }
        invalidateState()
    }

    // Has what up next added since looked up, one lookup at a time, and goes on until
    // [items] has caught up with the core, which only ever adds to the end.
    private fun growQueue() {
        val from = items.size
        val count = core.status.count
        if (growing || count <= from) return
        val uids = (from until count).map { core.queueUid(it).orEmpty() }
        val version = queueVersion
        growing = true
        addedItems(uids, items.firstOrNull()?.playOrigin ?: CorePlayer.Origin()) { found ->
            main.post {
                growing = false
                if (version == queueVersion && items.size == from && found.size == uids.size) {
                    items = items + found
                    orderVersion++ // its size has to be the timeline's
                    invalidateState()
                    onUpNextChanged() // the track either side may be known now
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

        val NEXT_SEEKS = setOf(COMMAND_SEEK_TO_NEXT, COMMAND_SEEK_TO_NEXT_MEDIA_ITEM)
        val CORE_SEEKS = NEXT_SEEKS + setOf(COMMAND_SEEK_TO_PREVIOUS, COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM)

        // The volume keys' steps on a remote device.
        const val VOLUME_STEPS = 20
        // How long the core stays muted once its sound is the phone's again: the core's thread
        // stops the device within its 40 ms wake.
        const val UNMUTE_MS = 250L

        val AUDIO_ATTRIBUTES: AudioAttributes = AudioAttributes.Builder()
            .setUsage(C.USAGE_MEDIA)
            .setContentType(C.AUDIO_CONTENT_TYPE_MUSIC)
            .build()
    }
}
