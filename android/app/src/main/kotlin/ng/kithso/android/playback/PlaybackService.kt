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
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService
import androidx.media3.session.SessionCommand
import androidx.media3.session.SessionResult
import ng.kithso.android.MainActivity
import ng.kithso.android.KithsongApp
import ng.kithso.android.R
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.cancel

/**
 * Plays in the background: the media session around [CoreMediaPlayer], which gives the
 * notification, lock screen and headset controls, and which the app's UI connects to with a
 * MediaController ([SessionPlayback]). Media3 makes it a foreground service while playing.
 * The notification and the system's media controls have a shuffle button beside next.
 */
@OptIn(UnstableApi::class)
class PlaybackService : MediaSessionService() {
    private val scope = MainScope()
    private var session: MediaSession? = null

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
        session = MediaSession.Builder(this, player)
            .setSessionActivity(openApp)
            .setBitmapLoader(ArtBitmapLoader(this, scope))
            .setCallback(Callback)
            .setMediaButtonPreferences(shuffleButtons(settings.shuffle))
            .build()
        player.addListener(object : Player.Listener {
            override fun onShuffleModeEnabledChanged(shuffleModeEnabled: Boolean) {
                session?.setMediaButtonPreferences(shuffleButtons(shuffleModeEnabled))
            }
        })
        setMediaNotificationProvider(
            DefaultMediaNotificationProvider.Builder(this).build().apply { setSmallIcon(R.drawable.ic_notification) },
        )
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

    private object Callback : MediaSession.Callback {
        // Every controller may use the shuffle button, the notification's among them.
        override fun onConnect(session: MediaSession, controller: MediaSession.ControllerInfo): MediaSession.ConnectionResult {
            val commands = if (controller.isTrusted) {
                MediaSession.ConnectionResult.DEFAULT_SESSION_COMMANDS
            } else {
                MediaSession.ConnectionResult.DEFAULT_UNTRUSTED_SESSION_COMMANDS
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
        // metadata (see toMediaItem), which CoreMediaPlayer reads, so take them as they are.
        override fun onAddMediaItems(
            mediaSession: MediaSession,
            controller: MediaSession.ControllerInfo,
            mediaItems: List<MediaItem>,
        ): ListenableFuture<List<MediaItem>> = Futures.immediateFuture(mediaItems)
    }

    private companion object {
        const val ACTION_SHUFFLE_ON = "ng.kithso.SHUFFLE_ON"
        const val ACTION_SHUFFLE_OFF = "ng.kithso.SHUFFLE_OFF"
        val SHUFFLE_ON = SessionCommand(ACTION_SHUFFLE_ON, Bundle.EMPTY)
        val SHUFFLE_OFF = SessionCommand(ACTION_SHUFFLE_OFF, Bundle.EMPTY)
    }
}
