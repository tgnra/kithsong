package ng.kithso.android.cast

import android.content.Context
import com.google.android.gms.cast.CastMediaControlIntent
import com.google.android.gms.cast.framework.CastOptions
import com.google.android.gms.cast.framework.OptionsProvider
import com.google.android.gms.cast.framework.SessionProvider
import com.google.android.gms.cast.framework.media.CastMediaOptions

/**
 * Google Cast's options, which the Cast library reads from the class the manifest names: the
 * Default Media Receiver, Google's own, so there's no receiver app of ours to host. The Cast
 * library's notification and media session are off, as PlaybackService's media session already
 * shows what plays, cast or not, on the notification and the lock screen. A session isn't
 * picked up again after the process has gone: the queue and place come back paused on the
 * phone, as they do without Cast. Ending a session stops the receiver, so nothing is left
 * playing on the device.
 */
class CastOptionsProvider : OptionsProvider {
    override fun getCastOptions(context: Context): CastOptions = CastOptions.Builder()
        .setReceiverApplicationId(CastMediaControlIntent.DEFAULT_MEDIA_RECEIVER_APPLICATION_ID)
        .setCastMediaOptions(
            CastMediaOptions.Builder()
                .setNotificationOptions(null)
                .setMediaSessionEnabled(false)
                .build(),
        )
        .setResumeSavedSession(false)
        .setStopReceiverApplicationWhenEndingSession(true)
        .build()

    override fun getAdditionalSessionProviders(context: Context): List<SessionProvider>? = null
}
