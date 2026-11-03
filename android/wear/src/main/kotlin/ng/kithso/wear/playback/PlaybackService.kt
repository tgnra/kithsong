package ng.kithso.wear.playback

import android.app.PendingIntent
import android.content.Intent
import androidx.annotation.OptIn
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.DefaultMediaNotificationProvider
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService
import ng.kithso.playback.CoreMediaPlayer
import ng.kithso.wear.KithsongApp
import ng.kithso.wear.MainActivity
import ng.kithso.wear.R

/**
 * Plays in the background: the media session around [CoreMediaPlayer], which the watch's own
 * media controls and headphones' buttons drive, and which the app's UI connects to with a
 * MediaController ([SessionPlayback]). Media3 makes it a foreground service while playing, so
 * the system leaves it be. Shuffle stays off and the core's volume at full: the watch has no
 * shuffle switch, and its volume is the system's.
 */
@OptIn(UnstableApi::class)
class PlaybackService : MediaSessionService() {
    private var session: MediaSession? = null

    override fun onCreate() {
        super.onCreate()
        val app = application as KithsongApp
        val player = CoreMediaPlayer(
            this,
            shuffle = false,
            volume = 100,
            onShuffleChanged = {},
            onNowPlaying = app.scrobbling::nowPlaying,
            onPlayed = app.scrobbling::played,
        )
        val openApp = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        session = MediaSession.Builder(this, player).setSessionActivity(openApp).build()
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
        super.onDestroy()
    }
}
