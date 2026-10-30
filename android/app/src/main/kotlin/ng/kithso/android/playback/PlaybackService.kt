package ng.kithso.android.playback

import android.app.PendingIntent
import android.content.Intent
import android.os.Bundle
import androidx.annotation.OptIn
import androidx.media3.common.MediaItem
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.CommandButton
import androidx.media3.session.DefaultMediaNotificationProvider
import androidx.media3.session.LibraryResult
import androidx.media3.session.MediaLibraryService
import androidx.media3.session.MediaLibraryService.LibraryParams
import androidx.media3.session.MediaLibraryService.MediaLibrarySession
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSession.MediaItemsWithStartPosition
import androidx.media3.session.SessionCommand
import androidx.media3.session.SessionError
import androidx.media3.session.SessionResult
import ng.kithso.android.MainActivity
import ng.kithso.android.KithsongApp
import ng.kithso.android.R
import com.google.common.collect.ImmutableList
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import com.google.common.util.concurrent.SettableFuture
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.launch

/**
 * Plays in the background: the media session around [CoreMediaPlayer], which gives the
 * notification, lock screen and headset controls, and which the app's UI connects to with a
 * MediaController ([SessionPlayback]). Media3 makes it a foreground service while playing.
 * The notification and the system's media controls have a shuffle button beside next.
 *
 * It's a library session, so Android Auto and other media browsers can browse the library
 * ([BrowseTree]) and play from it, by id or by voice search; the app's own queues come with
 * their files and are played as they are.
 */
@OptIn(UnstableApi::class)
class PlaybackService : MediaLibraryService() {
    private val scope = MainScope()
    private var session: MediaLibrarySession? = null

    override fun onCreate() {
        super.onCreate()
        val app = application as KithsongApp
        val settings = app.settings
        val player = CoreMediaPlayer(
            this,
            settings.shuffle,
            settings.volume,
            onShuffleChanged = { settings.shuffle = it },
            onNowPlaying = { trackUid -> app.scrobbling.nowPlaying(trackUid) },
        ) { trackUid, startedMs ->
            app.library.recordPlay(trackUid)
            app.scrobbling.scrobble(trackUid, startedMs)
        }
        val openApp = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val tree = BrowseTree(this) { app.library }
        val session = MediaLibrarySession.Builder(this, player, Callback(tree))
            .setSessionActivity(openApp)
            .setBitmapLoader(ArtBitmapLoader(this, scope))
            .setMediaButtonPreferences(shuffleButtons(settings.shuffle))
            .build()
        this.session = session
        player.addListener(object : Player.Listener {
            override fun onShuffleModeEnabledChanged(shuffleModeEnabled: Boolean) {
                session.setMediaButtonPreferences(shuffleButtons(shuffleModeEnabled))
            }
        })
        setMediaNotificationProvider(
            DefaultMediaNotificationProvider.Builder(this).build().apply { setSmallIcon(R.drawable.ic_notification) },
        )
        // So browsers show the tabs again after a sync or an import. The library swapped in
        // for the made-up one isn't followed, as it doesn't change.
        scope.launch {
            app.library.library.filterNotNull().collect { library ->
                tree.tabSizes(library).forEach { (id, size) -> session.notifyChildrenChanged(id, size, null) }
            }
        }
    }

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo) = session

    override fun onDestroy() {
        session?.run {
            player.release()
            release()
        }
        session = null
        scope.cancel()
        super.onDestroy()
    }

    // The shuffle button, which shows whether it's on and turns it the other way.
    private fun shuffleButtons(on: Boolean) = listOf(
        CommandButton.Builder(if (on) CommandButton.ICON_SHUFFLE_ON else CommandButton.ICON_SHUFFLE_OFF)
            .setDisplayName(getString(if (on) R.string.shuffle_on else R.string.shuffle_off))
            .setSessionCommand(if (on) SHUFFLE_OFF else SHUFFLE_ON)
            .setSlots(CommandButton.SLOT_FORWARD_SECONDARY, CommandButton.SLOT_OVERFLOW)
            .build(),
    )

    private inner class Callback(private val tree: BrowseTree) : MediaLibrarySession.Callback {
        // Every controller may use the shuffle button, the notification's among them, and
        // browse the library.
        override fun onConnect(session: MediaSession, controller: MediaSession.ControllerInfo): MediaSession.ConnectionResult {
            val commands = if (controller.isTrusted) {
                MediaSession.ConnectionResult.DEFAULT_SESSION_AND_LIBRARY_COMMANDS
            } else {
                MediaSession.ConnectionResult.DEFAULT_UNTRUSTED_SESSION_AND_LIBRARY_COMMANDS
            }
            return MediaSession.ConnectionResult.AcceptedResultBuilder(session, controller)
                .setAvailableSessionCommands(commands.buildUpon().add(SHUFFLE_ON).add(SHUFFLE_OFF).build())
                .build()
        }

        override fun onCustomCommand(
            session: MediaSession,
            controller: MediaSession.ControllerInfo,
            customCommand: SessionCommand,
            args: Bundle,
        ): ListenableFuture<SessionResult> {
            val on = when (customCommand.customAction) {
                ACTION_SHUFFLE_ON -> true
                ACTION_SHUFFLE_OFF -> false
                else -> return Futures.immediateFuture(SessionResult(SessionResult.RESULT_ERROR_NOT_SUPPORTED))
            }
            session.player.shuffleModeEnabled = on
            return Futures.immediateFuture(SessionResult(SessionResult.RESULT_SUCCESS))
        }

        // Items from a controller can arrive without their URI; it's also in the request
        // metadata (see toMediaItem), which CoreMediaPlayer reads, so the app's are taken as
        // they are. A browser's have only an id or a search query, and become the queue they
        // stand for.
        override fun onAddMediaItems(
            mediaSession: MediaSession,
            controller: MediaSession.ControllerInfo,
            mediaItems: List<MediaItem>,
        ): ListenableFuture<List<MediaItem>> {
            if (mediaItems.all { it.path != null }) return Futures.immediateFuture(mediaItems)
            return future {
                mediaItems.flatMap { item -> if (item.path != null) listOf(item) else tree.queue(item)?.mediaItems.orEmpty() }
            }
        }

        override fun onSetMediaItems(
            mediaSession: MediaSession,
            controller: MediaSession.ControllerInfo,
            mediaItems: List<MediaItem>,
            startIndex: Int,
            startPositionMs: Long,
        ): ListenableFuture<MediaItemsWithStartPosition> {
            if (mediaItems.all { it.path != null }) {
                return Futures.immediateFuture(MediaItemsWithStartPosition(mediaItems, startIndex, startPositionMs))
            }
            return future {
                // A browser plays one item at a time, so where it starts is the item's to say.
                mediaItems.singleOrNull()?.let { tree.queue(it) }
                    ?: throw UnsupportedOperationException("Nothing to play for ${mediaItems.map { it.mediaId }}")
            }
        }

        override fun onGetLibraryRoot(
            session: MediaLibrarySession,
            browser: MediaSession.ControllerInfo,
            params: LibraryParams?,
        ): ListenableFuture<LibraryResult<MediaItem>> =
            Futures.immediateFuture(LibraryResult.ofItem(tree.root, tree.rootParams))

        override fun onGetItem(
            session: MediaLibrarySession,
            browser: MediaSession.ControllerInfo,
            mediaId: String,
        ): ListenableFuture<LibraryResult<MediaItem>> = future {
            tree.item(mediaId)?.let { LibraryResult.ofItem(it, null) } ?: LibraryResult.ofError(SessionError.ERROR_BAD_VALUE)
        }

        override fun onGetChildren(
            session: MediaLibrarySession,
            browser: MediaSession.ControllerInfo,
            parentId: String,
            page: Int,
            pageSize: Int,
            params: LibraryParams?,
        ): ListenableFuture<LibraryResult<ImmutableList<MediaItem>>> = future {
            val children = tree.children(parentId) ?: return@future LibraryResult.ofError(SessionError.ERROR_BAD_VALUE)
            LibraryResult.ofItemList(children.page(page, pageSize), params)
        }

        // Searched once to say how many it found, then again for each page, as asked; the
        // core's search is quick.
        override fun onSearch(
            session: MediaLibrarySession,
            browser: MediaSession.ControllerInfo,
            query: String,
            params: LibraryParams?,
        ): ListenableFuture<LibraryResult<Void>> = future {
            if (query.isNotBlank()) session.notifySearchResultChanged(browser, query, tree.search(query).size, params)
            LibraryResult.ofVoid()
        }

        override fun onGetSearchResult(
            session: MediaLibrarySession,
            browser: MediaSession.ControllerInfo,
            query: String,
            page: Int,
            pageSize: Int,
            params: LibraryParams?,
        ): ListenableFuture<LibraryResult<ImmutableList<MediaItem>>> = future {
            LibraryResult.ofItemList(tree.search(query).page(page, pageSize), params)
        }
    }

    // [block] in the service's scope, on the main thread, for a callback's future.
    private fun <T> future(block: suspend CoroutineScope.() -> T): ListenableFuture<T> {
        val future = SettableFuture.create<T>()
        scope.launch {
            try {
                future.set(block())
            } catch (e: CancellationException) {
                future.cancel(false)
                throw e
            } catch (e: Exception) {
                future.setException(e)
            }
        }
        return future
    }

    private companion object {
        fun <T> List<T>.page(page: Int, pageSize: Int): List<T> {
            val from = page.toLong() * pageSize
            return if (from >= size) emptyList() else subList(from.toInt(), minOf(size.toLong(), from + pageSize).toInt())
        }

        const val ACTION_SHUFFLE_ON = "ng.kithso.SHUFFLE_ON"
        const val ACTION_SHUFFLE_OFF = "ng.kithso.SHUFFLE_OFF"
        val SHUFFLE_ON = SessionCommand(ACTION_SHUFFLE_ON, Bundle.EMPTY)
        val SHUFFLE_OFF = SessionCommand(ACTION_SHUFFLE_OFF, Bundle.EMPTY)
    }
}
