package ng.kithso.android.cast

import android.content.Context
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import androidx.annotation.OptIn
import androidx.media3.common.C
import androidx.media3.common.DeviceInfo
import androidx.media3.common.ForwardingSimpleBasePlayer
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import com.google.android.gms.cast.MediaInfo
import com.google.android.gms.cast.MediaLoadRequestData
import com.google.android.gms.cast.MediaMetadata
import com.google.android.gms.cast.MediaQueueData
import com.google.android.gms.cast.MediaQueueItem
import com.google.android.gms.cast.MediaSeekOptions
import com.google.android.gms.cast.MediaStatus
import com.google.android.gms.cast.framework.CastSession
import com.google.android.gms.cast.framework.media.RemoteMediaClient
import com.google.android.gms.common.api.PendingResult
import com.google.android.gms.common.images.WebImage
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import ng.kithso.core.CorePlayer
import ng.kithso.playback.CoreMediaPlayer
import ng.kithso.playback.path
import java.io.File
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * The media session's player while casting: [core] (CoreMediaPlayer) as it is, but playing on
 * the Cast device. PlaybackService gives it to the session in the core's place for as long as
 * a cast lasts ([begin] to [end]), so the notification, the lock screen, headset buttons and the
 * app all control the cast the same way they control the phone.
 *
 * The core stays what decides what plays: the queue, the shuffled order, stations and up next
 * are all its own, as on the phone. It's just silent ([CoreMediaPlayer.remote]), held paused on
 * the item the device plays, and this moves it along as the device goes on: next, previous and
 * the rest go to the core as they would, and whatever item the core is then on is loaded onto
 * the device. The device's queue is only ever that item and the one the core says comes after
 * it, which the device fetches ahead and goes on to by itself when the first ends, without a
 * gap of any length; this then takes the core on to it as well. If what comes next changes in
 * the meantime (up next, shuffle), the device's second item is swapped for the new one.
 *
 * What's playing, paused or not, and how far in, is the device's, so pausing or seeking from
 * another phone or the TV's remote shows here too. So does the device's volume, which the
 * session offers as a remote device's volume, for the volume keys and the system's output
 * switcher. Plays are counted as the device plays them ([CastPlays]).
 *
 * The device fetches each track from [CastServer], which only serves those two items. Use it on
 * the main thread.
 */
@OptIn(UnstableApi::class)
class CastPlayer(
    context: Context,
    private val core: CoreMediaPlayer,
    private val plays: CastPlays,
    private val scope: CoroutineScope,
    /** Stop was asked for while casting, which ends the cast; see CastController. */
    private val onStop: () -> Unit,
    /** Another app cast to the device over this one, which ends the cast here; see CastController. */
    private val onDisplaced: () -> Unit,
) : ForwardingSimpleBasePlayer(core) {
    /** A queue item of the core's as put on the device: [serial] tells them apart in its URL. */
    private class Entry(val serial: Int, val index: Int, val item: MediaItem, val file: File)

    // While casting.
    private var session: CastSession? = null
    private var client: RemoteMediaClient? = null
    private var server: CastServer? = null

    private val entries = LinkedHashMap<Int, Entry>()
    private var serial = 0
    // What the device plays (or is loading), and the item queued on it after that.
    private var current: Entry? = null
    private var queued: Entry? = null
    private var queueing = false
    // Requests sent and not yet answered: the device's status meanwhile can be from before them.
    private var requests = 0
    // Whether playing is wanted: the device's state once it says, what was asked for till then.
    private var wantPlaying = false
    private var wantReason = Player.PLAY_WHEN_READY_CHANGE_REASON_USER_REQUEST
    private var buffering = false
    // Items in a row the device couldn't play.
    private var failures = 0
    // How far into [current] it was, at [positionAt] (monotonic), going on from there if [positionPlaying].
    private var positionMs = 0L
    private var positionAt = 0L
    private var positionPlaying = false
    private var discontinuityMs: Long? = null
    // Another app's media has replaced this one's on the device.
    private var displaced = false
    // While this moves the core itself, so its events aren't taken for someone skipping.
    private var moving = false
    private var ticker: Job? = null

    /** The device's volume, 0 to 1, and whether it's muted, as it last said. */
    var volume = 0.0
        private set
    var muted = false
        private set

    private val wakeLock = (context.getSystemService(Context.POWER_SERVICE) as PowerManager)
        .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "kithsong:cast")
        .apply { setReferenceCounted(false) }
    @Suppress("DEPRECATION") // LOW_LATENCY only holds while the screen's on
    private val wifiLock = (context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager)
        .createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, "kithsong:cast")
        .apply { setReferenceCounted(false) }

    private val coreListener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) = onCoreChanged()
    }

    private val clientCallback = object : RemoteMediaClient.Callback() {
        override fun onStatusUpdated() = onDeviceStatus()
        override fun onQueueStatusUpdated() = onDeviceStatus()
        override fun onPreloadStatusUpdated() = onDeviceStatus()
    }

    /** Whether a cast is under way: between [begin] and [end]. */
    val casting: Boolean get() = session != null

    /**
     * Whether the device still has this cast's media, or nothing else: if another app has cast
     * over it, ending the cast mustn't stop the receiver under them.
     */
    val ownsDevice: Boolean get() = casting && !displaced

    /**
     * Starts casting to [session], whose media client is [client], with the tracks served by
     * [server]: what the core has playing moves to the device, at the same place, playing or
     * paused as it was. Nothing in the queue, nothing's loaded.
     */
    fun begin(session: CastSession, client: RemoteMediaClient, server: CastServer) {
        this.session = session
        this.client = client
        this.server = server
        displaced = false
        val playing = core.playWhenReady && core.playbackState != Player.STATE_IDLE
        moving = true
        try {
            core.playWhenReady = false
            // The play the phone was in the middle of ends here, with what the phone played of
            // it. Then a row of the core's for the rest, which the device plays.
            core.endPlay()
            plays.casting = true
            core.remote = true
            if (core.mediaItemCount > 0) {
                if (core.playbackState == Player.STATE_IDLE) core.prepare()
                core.startPaused()
                // A seek with no play under way starts one, at the same place.
                core.seekTo(core.currentPosition)
            }
        } finally {
            moving = false
        }
        core.addListener(coreListener)
        client.registerCallback(clientCallback)
        updateVolume()
        wantPlaying = false
        if (core.mediaItemCount > 0) load(core.currentPosition, playing)
        invalidateState()
    }

    /**
     * Stops casting, leaving the core on the item the device was on, at the place it had got to,
     * paused. The device isn't told anything: the session is ending, or has.
     */
    fun end() {
        if (!casting) return
        val position = deviceLastPositionMs()
        plays.end(CorePlayer.PlayEnd.Stopped)
        client?.unregisterCallback(clientCallback)
        core.removeListener(coreListener)
        session = null
        client = null
        server = null
        current = null
        queued = null
        entries.clear()
        requests = 0
        queueing = false
        wantPlaying = false
        buffering = false
        hold(false)
        moving = true
        try {
            // The core's rows of the cast, with what the device played of them; after this,
            // the phone's own.
            core.endPlay()
            plays.casting = false
            plays.clear()
            core.remote = false
            // Only if it moved: a seek starts a play of the phone's, which only playing should.
            if (core.mediaItemCount > 0 && abs(position - core.currentPosition) > SAME_PLACE_MS) core.seekTo(position)
        } finally {
            moving = false
        }
        invalidateState()
    }

    /** Up next changed, so what comes next may have; the core's own events don't say. */
    fun queueChanged() = onCoreChanged()

    /** The device's volume changed. */
    fun updateVolume() {
        val session = session ?: return
        try {
            volume = session.volume
            muted = session.isMute
        } catch (e: IllegalStateException) {
            return
        }
        invalidateState()
    }

    /** Sets the device's volume, 0 to 1. */
    fun setCastVolume(volume: Double) {
        val session = session ?: return
        try {
            session.volume = volume.coerceIn(0.0, 1.0)
            this.volume = volume.coerceIn(0.0, 1.0)
        } catch (e: Exception) {
            Log.w(TAG, "Couldn't set the cast volume", e)
        }
        invalidateState()
    }

    override fun getState(): State {
        val state = super.getState()
        if (!casting) return state
        val empty = state.playlist.isEmpty()
        val builder = state.buildUpon()
            .setPlayWhenReady(wantPlaying, wantReason)
            .setPlaybackState(
                when {
                    empty || state.playbackState == Player.STATE_IDLE -> Player.STATE_IDLE
                    buffering && current != null -> Player.STATE_BUFFERING
                    else -> Player.STATE_READY
                },
            )
            .setPlaybackSuppressionReason(Player.PLAYBACK_SUPPRESSION_REASON_NONE)
            .setContentPositionMs { position() }
            .setDeviceInfo(DeviceInfo.Builder(DeviceInfo.PLAYBACK_TYPE_REMOTE).setMinVolume(0).setMaxVolume(VOLUME_STEPS).build())
            .setDeviceVolume((volume * VOLUME_STEPS).roundToInt().coerceIn(0, VOLUME_STEPS))
            .setIsDeviceMuted(muted)
            .setAvailableCommands(
                state.availableCommands.buildUpon().addAll(
                    Player.COMMAND_GET_DEVICE_VOLUME,
                    Player.COMMAND_SET_DEVICE_VOLUME_WITH_FLAGS,
                    Player.COMMAND_ADJUST_DEVICE_VOLUME_WITH_FLAGS,
                ).build(),
            )
        discontinuityMs?.let {
            builder.setPositionDiscontinuity(Player.DISCONTINUITY_REASON_SEEK, it)
            discontinuityMs = null
        }
        return builder.build()
    }

    override fun handleSetPlayWhenReady(playWhenReady: Boolean): ListenableFuture<*> {
        if (!casting) return super.handleSetPlayWhenReady(playWhenReady)
        val client = client ?: return done()
        wantPlaying = playWhenReady
        wantReason = Player.PLAY_WHEN_READY_CHANGE_REASON_USER_REQUEST
        when {
            playWhenReady && current == null -> startCurrent()
            current == null -> Unit
            playWhenReady -> send(client.play())
            else -> send(client.pause())
        }
        onPlayingChanged()
        return done()
    }

    override fun handleSeek(mediaItemIndex: Int, positionMs: Long, seekCommand: Int): ListenableFuture<*> {
        if (!casting) return super.handleSeek(mediaItemIndex, positionMs, seekCommand)
        val position = if (positionMs == C.TIME_UNSET) 0 else positionMs
        // Skips go to the core, whose order says where they go, and whatever it's on then
        // is loaded (onCoreChanged). Seeks in the track go to the device as well.
        when (seekCommand) {
            Player.COMMAND_SEEK_TO_NEXT, Player.COMMAND_SEEK_TO_NEXT_MEDIA_ITEM -> core.skipToNext()
            Player.COMMAND_SEEK_TO_PREVIOUS_MEDIA_ITEM -> core.seekToPreviousMediaItem()
            // Already decided by the device's position: back to the start, or the item before.
            Player.COMMAND_SEEK_TO_PREVIOUS ->
                if (mediaItemIndex == core.currentMediaItemIndex) seekInTrack(position) else core.seekToPreviousMediaItem()
            else ->
                if (mediaItemIndex == C.INDEX_UNSET || mediaItemIndex == core.currentMediaItemIndex) seekInTrack(position)
                else core.seekTo(mediaItemIndex, position)
        }
        return done()
    }

    override fun handleSetMediaItems(mediaItems: List<MediaItem>, startIndex: Int, startPositionMs: Long): ListenableFuture<*> {
        if (!casting) return super.handleSetMediaItems(mediaItems, startIndex, startPositionMs)
        if (current != null) plays.end(CorePlayer.PlayEnd.Replaced)
        val wasLoaded = current != null
        current = null
        queued = null
        moving = true
        try {
            super.handleSetMediaItems(mediaItems, startIndex, startPositionMs)
        } finally {
            moving = false
        }
        // A new queue plays at once if playing was wanted, as on the phone; else the old one
        // goes from the device.
        if (wantPlaying && core.mediaItemCount > 0) startCurrent() else if (wasLoaded) client?.let { send(it.stop()) }
        invalidateState()
        return done()
    }

    // Stopping, as the app does when it's left with nothing playing, leaves nothing running:
    // the cast ends too. Once the session has done with this call, as the cast's end swaps
    // the session's player.
    override fun handleStop(): ListenableFuture<*> {
        if (!casting) return super.handleStop()
        scope.launch { if (casting) onStop() }
        return done()
    }

    // Only ever let go of when the service goes, after the core: the core is the service's.
    override fun handleRelease(): ListenableFuture<*> {
        end()
        return done()
    }

    override fun handleSetDeviceVolume(deviceVolume: Int, flags: Int): ListenableFuture<*> {
        setCastVolume(deviceVolume.toDouble() / VOLUME_STEPS)
        return done()
    }

    override fun handleIncreaseDeviceVolume(flags: Int): ListenableFuture<*> {
        setCastVolume(((volume * VOLUME_STEPS).roundToInt() + 1).toDouble() / VOLUME_STEPS)
        return done()
    }

    override fun handleDecreaseDeviceVolume(flags: Int): ListenableFuture<*> {
        setCastVolume(((volume * VOLUME_STEPS).roundToInt() - 1).toDouble() / VOLUME_STEPS)
        return done()
    }

    override fun handleSetDeviceMuted(muted: Boolean, flags: Int): ListenableFuture<*> {
        try {
            session?.setMute(muted)
            this.muted = muted
        } catch (e: Exception) {
            Log.w(TAG, "Couldn't mute the cast", e)
        }
        invalidateState()
        return done()
    }

    // The core's current item, from where it is, loaded onto the device: nothing having been,
    // or it having stopped.
    private fun startCurrent() {
        if (core.mediaItemCount == 0) return
        moving = true
        try {
            if (core.playbackState == Player.STATE_IDLE) core.prepare()
            core.startPaused()
        } finally {
            moving = false
        }
        load(core.currentPosition, play = true)
    }

    // Loads the core's current item onto the device at [positionMs], with the item after it.
    private fun load(positionMs: Long, play: Boolean) {
        val client = client ?: return
        val index = core.indexAt(0)
        val item = core.itemAt(0)
        current = null
        queued = null
        wantPlaying = play
        if (item == null) {
            // Still being looked up (a station's, or up next's): loaded once it's known.
            invalidateState()
            return
        }
        val entry = entryFor(index, item)
        if (entry == null) {
            // No file here any more: the core skips such an item, and so does this.
            failed()
            return
        }
        current = entry
        queued = nextEntry()
        setPosition(positionMs, playing = false)
        val queue = listOfNotNull(entry, queued).map(::queueItem)
        serveOnly()
        val request = MediaLoadRequestData.Builder()
            .setQueueData(MediaQueueData.Builder().setItems(queue).setStartIndex(0).build())
            .setAutoplay(play)
            .setCurrentTime(positionMs)
            .build()
        send(client.load(request))
        plays.start(item.mediaId, item.mediaMetadata.durationMs ?: 0)
        onPlayingChanged()
        invalidateState()
    }

    // A move to a place in the current track. Back to the start is playing it again, as the
    // core has it, so a new play.
    private fun seekInTrack(position: Long) {
        val current = current
        if (position == 0L && current != null) {
            plays.end(CorePlayer.PlayEnd.Skipped)
            plays.start(current.item.mediaId, current.item.mediaMetadata.durationMs ?: 0)
        }
        moving = true
        try {
            core.seekTo(position)
        } finally {
            moving = false
        }
        val client = client
        if (current != null && client != null) {
            send(client.seek(MediaSeekOptions.Builder().setPosition(position).build()))
            setPosition(position, positionPlaying)
        }
        invalidateState()
    }

    // The core moved: if it's to another item than the device's, someone skipped, and the
    // device plays what the core is on now. Else what comes next may have changed.
    private fun onCoreChanged() {
        if (moving || !casting) return
        if (core.mediaItemCount == 0) {
            if (current != null) {
                plays.end(CorePlayer.PlayEnd.Stopped)
                current = null
                queued = null
                client?.let { send(it.stop()) }
            }
            return
        }
        val current = current
        if (current == null) {
            // Waiting for the item to be looked up.
            if (wantPlaying && core.itemAt(0) != null && core.playbackState != Player.STATE_IDLE) load(core.currentPosition, true)
            return
        }
        if (core.indexAt(0) != current.index || core.itemAt(0)?.mediaId != current.item.mediaId) {
            plays.end(CorePlayer.PlayEnd.Skipped)
            load(core.currentPosition, wantPlaying)
        } else {
            syncNext()
        }
    }

    // What the device says it's doing.
    private fun onDeviceStatus() {
        val client = client ?: return
        val status = client.mediaStatus
        val on = entryOf(status?.mediaInfo)
        val current = current
        when {
            current == null || status == null -> Unit
            // It went on to the item queued after this one, by itself.
            on != null && on.serial > current.serial -> advanced(on)
            requests > 0 -> Unit
            on === current -> when (status.playerState) {
                MediaStatus.PLAYER_STATE_PLAYING -> {
                    buffering = false
                    failures = 0
                    adopt(playing = true)
                }
                MediaStatus.PLAYER_STATE_PAUSED -> {
                    buffering = false
                    adopt(playing = false)
                }
                MediaStatus.PLAYER_STATE_BUFFERING, MediaStatus.PLAYER_STATE_LOADING -> buffering = true
                MediaStatus.PLAYER_STATE_IDLE -> idle(
                    status,
                    hasNext = status.loadingItemId != MediaQueueItem.INVALID_ITEM_ID || idsAfter(status.currentItemId).isNotEmpty(),
                )
            }
            // Its queue played out, which leaves no media.
            on == null && status.mediaInfo == null && status.playerState == MediaStatus.PLAYER_STATE_IDLE -> idle(status, hasNext = false)
            // Something else is on the device: another app cast to it. Paused here, and the cast
            // ends, leaving the device to them; after the device's callback, as that swaps players.
            on == null && status.mediaInfo != null -> {
                displaced = true
                dropped()
                scope.launch { if (casting) onDisplaced() }
            }
        }
        val now = this.current
        if (now != null && entryOf(client.mediaStatus?.mediaInfo) === now) {
            val playing = client.mediaStatus?.playerState == MediaStatus.PLAYER_STATE_PLAYING
            val position = client.approximateStreamPosition
            // Moved by someone else: controllers are told, as they don't follow the position.
            if (requests == 0 && abs(position - expectedPositionMs()) > JUMP_MS) discontinuityMs = position
            setPosition(position, playing)
        } else {
            plays.update(playing = false)
        }
        invalidateState()
        syncNext()
    }

    private fun idle(status: MediaStatus, hasNext: Boolean) {
        when (status.idleReason) {
            MediaStatus.IDLE_REASON_FINISHED -> if (!hasNext) ended()
            MediaStatus.IDLE_REASON_ERROR -> failed()
            // Stopped from elsewhere, or replaced by another app's media.
            MediaStatus.IDLE_REASON_CANCELED, MediaStatus.IDLE_REASON_INTERRUPTED -> dropped()
        }
    }

    // Playing or paused, as the device now says, from whoever asked it.
    private fun adopt(playing: Boolean) {
        if (playing == wantPlaying) return
        wantPlaying = playing
        wantReason = Player.PLAY_WHEN_READY_CHANGE_REASON_REMOTE
        onPlayingChanged()
    }

    // The device went on to [on], as queued: the core goes on too, and if that's to something
    // else after all (what was next changed just then), that plays instead.
    private fun advanced(on: Entry) {
        plays.end(CorePlayer.PlayEnd.Finished)
        current = on
        queued = null
        moveCoreOn()
        if (core.indexAt(0) == on.index && core.itemAt(0)?.mediaId == on.item.mediaId) {
            setPosition(0, positionPlaying)
            plays.start(on.item.mediaId, on.item.mediaMetadata.durationMs ?: 0)
            serveOnly()
        } else {
            load(0, wantPlaying)
        }
    }

    // The device played its last item: on to what the core has next if there's more (what the
    // device didn't have yet), else stopped at the start, as when the queue plays out on the phone.
    private fun ended() {
        plays.end(CorePlayer.PlayEnd.Finished)
        current = null
        queued = null
        failures = 0
        if (core.isCommandAvailable(Player.COMMAND_SEEK_TO_NEXT_MEDIA_ITEM)) {
            moveCoreOn()
            load(0, play = true)
        } else {
            playedOut()
        }
    }

    // The device couldn't play the current item: on to the next, as the core does, unless it's
    // failing them all.
    private fun failed() {
        plays.end(CorePlayer.PlayEnd.Failed)
        current = null
        queued = null
        if (++failures > MAX_FAILURES || !core.isCommandAvailable(Player.COMMAND_SEEK_TO_NEXT_MEDIA_ITEM)) {
            Log.w(TAG, "The Cast device couldn't play the queue")
            failures = 0
            playedOut()
            return
        }
        moveCoreOn()
        load(0, play = true)
    }

    // Stopped from elsewhere: paused here, at the place it got to, to load again on play.
    private fun dropped() {
        plays.end(CorePlayer.PlayEnd.Stopped)
        // Where it was as last heard: the device's own may be reset by now.
        val position = expectedPositionMs()
        current = null
        queued = null
        moving = true
        try {
            // The core's play ends with the device's, and the seek starts the one playing
            // again would be, as after a stop on the phone.
            core.endPlay()
            core.seekTo(position)
        } finally {
            moving = false
        }
        setPosition(position, playing = false)
        adopt(playing = false)
    }

    private fun playedOut() {
        moving = true
        try {
            core.seekTo(0, 0)
        } finally {
            moving = false
        }
        setPosition(0, playing = false)
        wantPlaying = false
        wantReason = Player.PLAY_WHEN_READY_CHANGE_REASON_END_OF_MEDIA_ITEM
        onPlayingChanged()
    }

    private fun moveCoreOn() {
        moving = true
        try {
            // By the core's order: Media3's timeline may have no next where the core does.
            core.skipToNext()
        } finally {
            moving = false
        }
    }

    // Makes the device's queue the current item and the one the core has after it: the old
    // second item goes, the new one's added. Only once the device is on the current item, and
    // a change at a time.
    private fun syncNext() {
        val client = client ?: return
        val current = current ?: return
        if (queueing || requests > 0) return
        val status = client.mediaStatus ?: return
        if (entryOf(status.mediaInfo) !== current) return
        val want = nextEntry()
        if (want?.serial == queued?.serial) return
        val after = idsAfter(itemIdOf(status, current))
        queued = want
        serveOnly()
        queueing = true
        fun append() {
            if (want == null || this.current !== current) {
                queueing = false
                syncNext()
                return
            }
            client.queueAppendItem(queueItem(want), null).setResultCallback { result ->
                queueing = false
                if (!result.status.isSuccess) Log.w(TAG, "The Cast device didn't take the next track: ${result.status}")
                syncNext()
            }
        }
        if (after.isEmpty()) {
            append()
        } else {
            client.queueRemoveItems(after.toIntArray(), null).setResultCallback { append() }
        }
    }

    // The item the core has after the current one, as an entry: the one queued already, if it's
    // the same. Null for none, one still being looked up, or one with no file.
    private fun nextEntry(): Entry? {
        val index = core.indexAt(1)
        val item = core.itemAt(1) ?: return null
        val queued = queued
        if (queued != null && queued.index == index && queued.item.mediaId == item.mediaId) return queued
        return entryFor(index, item)
    }

    private fun entryFor(index: Int, item: MediaItem): Entry? {
        val file = item.path?.let(::File) ?: return null
        val entry = Entry(++serial, index, item, file)
        entries[entry.serial] = entry
        while (entries.size > MAX_ENTRIES) entries.remove(entries.keys.first())
        return entry
    }

    // The entry a device's media is, if it's one of this cast's.
    private fun entryOf(info: MediaInfo?): Entry? {
        val server = server ?: return null
        val name = server.nameOf(info?.contentId) ?: server.nameOf(info?.contentUrl) ?: return null
        if (!name.startsWith(TRACK)) return null
        return name.removePrefix(TRACK).substringBefore('.').toIntOrNull()?.let(entries::get)
    }

    // The device's queue item id for [entry]. Not simply its currentItemId: while it loads the
    // item it's going on to, the status has that item's media (its extended status) but still
    // the finished item's id as current, and the item after that would be the one loading.
    private fun itemIdOf(status: MediaStatus, entry: Entry): Int =
        status.queueItems.firstOrNull { entryOf(it.media) === entry }?.itemId
            ?: status.loadingItemId.takeIf { it != MediaQueueItem.INVALID_ITEM_ID }
            ?: status.currentItemId

    // The device's queue items after [itemId], which it has queued to play next.
    private fun idsAfter(itemId: Int): List<Int> {
        val ids = client?.mediaStatus?.queueItems?.map { it.itemId }?.takeIf { it.isNotEmpty() }
            ?: client?.mediaQueue?.itemIds?.toList() ?: return emptyList()
        val at = ids.indexOf(itemId)
        return if (at < 0) emptyList() else ids.drop(at + 1)
    }

    private fun trackName(entry: Entry) = "$TRACK${entry.serial}.${entry.file.extension.lowercase().ifEmpty { "bin" }}"
    private fun artName(entry: Entry) = "$ART${entry.serial}.jpg"

    // Serves only what's on the device.
    private fun serveOnly() {
        server?.keepOnly(listOfNotNull(current, queued).flatMap { listOf(trackName(it), artName(it)) }.toSet())
    }

    private fun queueItem(entry: Entry): MediaQueueItem {
        val server = server ?: throw IllegalStateException("Not casting")
        val url = server.add(trackName(entry), CastServer.Served.Track(entry.file))
        val metadata = entry.item.mediaMetadata
        val castMetadata = MediaMetadata(MediaMetadata.MEDIA_TYPE_MUSIC_TRACK).apply {
            metadata.title?.let { putString(MediaMetadata.KEY_TITLE, it.toString()) }
            metadata.artist?.let { putString(MediaMetadata.KEY_ARTIST, it.toString()) }
            metadata.albumTitle?.let { putString(MediaMetadata.KEY_ALBUM_TITLE, it.toString()) }
            metadata.trackNumber?.let { putInt(MediaMetadata.KEY_TRACK_NUMBER, it) }
            // ArtProvider's art, as the notification and Android Auto have it.
            metadata.artworkUri?.let { artwork ->
                addImage(WebImage(Uri.parse(server.add(artName(entry), CastServer.Served.Art(artwork)))))
            }
        }
        val info = MediaInfo.Builder(url)
            .setContentUrl(url)
            .setStreamType(MediaInfo.STREAM_TYPE_BUFFERED)
            .setContentType(CastServer.mimeType(entry.file.name))
            .setMetadata(castMetadata)
            .apply { metadata.durationMs?.let { setStreamDuration(it) } }
            .build()
        return MediaQueueItem.Builder(info).setPreloadTime(PRELOAD_S).build()
    }

    private fun send(result: PendingResult<RemoteMediaClient.MediaChannelResult>) {
        requests++
        result.setResultCallback { answer ->
            if (requests > 0) requests--
            if (!answer.status.isSuccess) Log.w(TAG, "The Cast device refused a request: ${answer.status}")
            onDeviceStatus()
        }
    }

    // Playing changed, by request or by the device: the wake and Wi-Fi locks, the ticker for
    // counting plays, and CastController's pause timer (through the state).
    private fun onPlayingChanged() {
        val playing = wantPlaying && current != null
        hold(playing)
        ticker?.cancel()
        ticker = if (!playing) null else scope.launch {
            while (isActive) {
                delay(TICK_MS)
                val client = client ?: break
                val on = current != null && entryOf(client.mediaStatus?.mediaInfo) === current
                plays.update(playing = on && client.mediaStatus?.playerState == MediaStatus.PLAYER_STATE_PLAYING)
            }
        }
        invalidateState()
    }

    private fun hold(playing: Boolean) {
        if (playing) {
            wakeLock.acquire()
            wifiLock.acquire()
        } else {
            if (wakeLock.isHeld) wakeLock.release()
            if (wifiLock.isHeld) wifiLock.release()
        }
    }

    private fun setPosition(position: Long, playing: Boolean) {
        positionMs = position
        positionAt = SystemClock.elapsedRealtime()
        positionPlaying = playing
        plays.update(playing)
    }

    private fun expectedPositionMs(): Long =
        positionMs + if (positionPlaying) SystemClock.elapsedRealtime() - positionAt else 0

    // The position the session shows: the device's, while it's on the current item.
    private fun position(): Long {
        val current = current ?: return core.currentPosition
        val client = client ?: return expectedPositionMs()
        return if (entryOf(client.mediaStatus?.mediaInfo) === current) client.approximateStreamPosition else positionMs
    }

    // Where the device had got to in the current item, as near as can be told.
    private fun deviceLastPositionMs(): Long {
        val current = current ?: return core.currentPosition
        val client = client
        if (client != null && entryOf(client.mediaStatus?.mediaInfo) === current) {
            return try {
                client.approximateStreamPosition
            } catch (e: Exception) {
                expectedPositionMs()
            }
        }
        return expectedPositionMs()
    }

    private fun done(): ListenableFuture<*> = Futures.immediateVoidFuture()

    private companion object {
        const val TAG = "kithsong"
        const val TRACK = "t"
        const val ART = "a"
        // The device fetches the next item this long before the current one ends.
        const val PRELOAD_S = 20.0
        const val MAX_ENTRIES = 16
        const val MAX_FAILURES = 3
        const val TICK_MS = 1000L
        // A position this far from where it should be has been moved.
        const val JUMP_MS = 2500L
        // Positions this close are the same place, for handing back.
        const val SAME_PLACE_MS = 1000L
        // The volume keys' steps.
        const val VOLUME_STEPS = 20
    }
}
