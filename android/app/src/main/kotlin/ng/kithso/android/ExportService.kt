package ng.kithso.android

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.text.format.Formatter
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import ng.kithso.android.data.ExportProgress

/**
 * Keeps the process running while [NewLibrary] exports the library, as ImportService does for
 * an import: a foreground service of the dataSync type with a progress notification and
 * Cancel, and a wake lock so it carries on with the screen off. A big library takes a while.
 * It stops once the export is over; the app says how it went.
 */
class ExportService : Service() {
    private val scope = MainScope()
    private var wakeLock: PowerManager.WakeLock? = null
    private var foreground = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val newLibrary = (application as KithsongApp).newLibrary
        if (intent?.action == ACTION_CANCEL) newLibrary.cancel()
        if (!foreground) {
            foreground = true
            createChannel(this)
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                notification(null),
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC else 0,
            )
            wakeLock = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "kithsong:export")
                .apply { acquire(WAKE_LOCK_TIMEOUT_MS) }
            scope.launch {
                var shown = -1
                newLibrary.state.collect { state ->
                    if (state !is NewLibrary.State.Exporting) {
                        stop()
                        return@collect
                    }
                    // A notification per percent, not per few MB.
                    val percent = state.progress?.let { (it.fraction * 100).toInt() } ?: -1
                    if (percent == shown) return@collect
                    shown = percent
                    NotificationManagerCompat.from(this@ExportService).notifyIfAllowed(notification(state))
                }
            }
        }
        return START_NOT_STICKY
    }

    // Android 15 limits data sync services to 6 hours a day.
    override fun onTimeout(startId: Int, fgsType: Int) {
        (application as KithsongApp).newLibrary.cancel()
        stop()
    }

    override fun onDestroy() {
        wakeLock?.takeIf { it.isHeld }?.release()
        scope.cancel()
        super.onDestroy()
    }

    private fun stop() {
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun notification(state: NewLibrary.State.Exporting?): android.app.Notification {
        val progress = state?.progress
        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(getString(R.string.export_title))
            .setContentText(progress?.let { exportProgressText(this, it) } ?: getString(R.string.export_listing))
            .setContentIntent(openApp())
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .setCategory(NotificationCompat.CATEGORY_PROGRESS)
            .addAction(0, getString(R.string.cancel), cancelAction())
        if (progress != null) builder.setProgress(PROGRESS_MAX, (progress.fraction * PROGRESS_MAX).toInt(), false)
        else builder.setProgress(0, 0, true)
        return builder.build()
    }

    private fun openApp() = PendingIntent.getActivity(
        this,
        0,
        Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )

    private fun cancelAction() = PendingIntent.getService(
        this,
        ACTION_CANCEL.hashCode(),
        Intent(this, ExportService::class.java).setAction(ACTION_CANCEL),
        PendingIntent.FLAG_IMMUTABLE,
    )

    private fun NotificationManagerCompat.notifyIfAllowed(notification: android.app.Notification) {
        if (areNotificationsEnabled()) {
            try {
                notify(NOTIFICATION_ID, notification)
            } catch (e: SecurityException) {
                Log.w(TAG, "Can't post the export notification", e)
            }
        }
    }

    companion object {
        private const val TAG = "kithsong"
        private const val CHANNEL_ID = "export"
        private const val NOTIFICATION_ID = 4 // ImportService has 2 and 3
        private const val ACTION_CANCEL = "ng.kithso.export.CANCEL"
        private const val PROGRESS_MAX = 1000
        // A safety net: the service releases it when the export is over.
        private const val WAKE_LOCK_TIMEOUT_MS = 6 * 60 * 60 * 1000L

        /**
         * Starts the service for an export that has just begun. If the system won't let it
         * start, the export still runs, just without the notification, for as long as the
         * process does.
         */
        fun start(context: Context) {
            try {
                ContextCompat.startForegroundService(context, Intent(context, ExportService::class.java))
            } catch (e: IllegalStateException) {
                // ForegroundServiceStartNotAllowedException, from Android 12, is one of these.
                Log.w(TAG, "Couldn't start the export service", e)
            }
        }

        private fun createChannel(context: Context) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
            val channel = NotificationChannel(CHANNEL_ID, context.getString(R.string.export_channel), NotificationManager.IMPORTANCE_LOW)
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
    }
}

/** "Zip file 1 of 2: 1.2 GB of 30 GB", as the notification and the export dialog say it. */
fun exportProgressText(context: Context, progress: ExportProgress): String = context.getString(
    R.string.export_progress,
    progress.zip,
    progress.zips,
    Formatter.formatShortFileSize(context, progress.bytesDone),
    Formatter.formatShortFileSize(context, progress.bytes),
)
