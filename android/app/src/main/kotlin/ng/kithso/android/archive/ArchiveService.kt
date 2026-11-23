package ng.kithso.android.archive

import android.app.Notification
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
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.app.NotificationManagerCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import ng.kithso.android.KithsongApp
import ng.kithso.android.MainActivity
import ng.kithso.android.R

/**
 * Keeps the process running while [LibraryArchiver] exports or restores, as ImportService does
 * for an import: a dataSync foreground service with a progress notification that can cancel,
 * and a wake lock. It stops once that's over, leaving the outcome if the app isn't on screen.
 */
class ArchiveService : Service() {
    private val scope = MainScope()
    private var wakeLock: PowerManager.WakeLock? = null
    private var foreground = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val app = application as KithsongApp
        val archiver = app.archiver
        if (intent?.action == ACTION_CANCEL) archiver.cancel()
        if (!foreground) {
            foreground = true
            createChannel(this)
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                progressNotification(archiver.job.value ?: ArchiveJob(restoring = false)),
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC else 0,
            )
            wakeLock = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "kithsong:archive")
                .apply { acquire(WAKE_LOCK_TIMEOUT_MS) }
            scope.launch {
                archiver.job.collect { job ->
                    if (job == null || job.isOver) {
                        if (job != null && !app.isVisible) showOutcome(this@ArchiveService, job)
                        stop()
                    } else {
                        notifyIfAllowed(this@ArchiveService, NOTIFICATION_ID, progressNotification(job))
                    }
                }
            }
        }
        return START_NOT_STICKY
    }

    // Android 15 limits data sync services to 6 hours a day.
    override fun onTimeout(startId: Int, fgsType: Int) {
        (application as KithsongApp).archiver.cancel()
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

    private fun progressNotification(job: ArchiveJob): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(job.title(this))
            .setContentIntent(openApp(this))
            .setProgress(100, job.percent, false)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .setCategory(NotificationCompat.CATEGORY_PROGRESS)
            .addAction(
                0,
                getString(R.string.cancel),
                PendingIntent.getService(
                    this, 0, Intent(this, ArchiveService::class.java).setAction(ACTION_CANCEL), PendingIntent.FLAG_IMMUTABLE,
                ),
            )
            .build()

    companion object {
        private const val TAG = "kithsong"
        private const val CHANNEL_ID = "archive"
        private const val NOTIFICATION_ID = 6
        private const val OUTCOME_ID = 7
        private const val ACTION_CANCEL = "ng.kithso.archive.CANCEL"
        // A safety net: the service releases it when the export or restore is over.
        private const val WAKE_LOCK_TIMEOUT_MS = 6 * 60 * 60 * 1000L

        /** Starts the service; if the system won't let it start, the work still runs while the process does. */
        fun start(context: Context) {
            try {
                ContextCompat.startForegroundService(context, Intent(context, ArchiveService::class.java))
            } catch (e: IllegalStateException) {
                Log.w(TAG, "Couldn't start the archive service", e)
            }
        }

        /** Says how [job] ended, in a notification that opens the app. */
        fun showOutcome(context: Context, job: ArchiveJob) {
            createChannel(context)
            val notification = NotificationCompat.Builder(context, CHANNEL_ID)
                .setSmallIcon(R.drawable.ic_notification)
                .setContentTitle(job.title(context))
                .setContentIntent(openApp(context))
                .setAutoCancel(true)
                .build()
            notifyIfAllowed(context, OUTCOME_ID, notification)
        }

        private fun openApp(context: Context) = PendingIntent.getActivity(
            context,
            0,
            Intent(context, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )

        private fun notifyIfAllowed(context: Context, id: Int, notification: Notification) {
            val manager = NotificationManagerCompat.from(context)
            if (!manager.areNotificationsEnabled()) return
            try {
                manager.notify(id, notification)
            } catch (e: SecurityException) {
                Log.w(TAG, "Can't post the archive notification", e)
            }
        }

        private fun createChannel(context: Context) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
            val channel = NotificationChannel(CHANNEL_ID, context.getString(R.string.archive_channel), NotificationManager.IMPORTANCE_LOW)
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
    }
}

/** What [this] is doing, or how it ended: a notification's title, or the line in Settings. */
fun ArchiveJob.title(context: Context): String = when (result) {
    null -> context.getString(if (restoring) R.string.library_restoring else R.string.library_exporting, percent)
    ArchiveJob.Result.Done -> context.getString(if (restoring) R.string.library_restored else R.string.library_exported)
    ArchiveJob.Result.Cancelled ->
        context.getString(if (restoring) R.string.library_restore_cancelled else R.string.library_export_cancelled)
    ArchiveJob.Result.NoSpace ->
        context.getString(if (restoring) R.string.library_restore_no_space else R.string.library_export_no_space)
    ArchiveJob.Result.TooBig -> context.getString(R.string.library_export_too_big)
    ArchiveJob.Result.Newer -> context.getString(R.string.library_restore_newer)
    ArchiveJob.Result.Bad -> context.getString(R.string.library_restore_bad)
    ArchiveJob.Result.Failed ->
        context.getString(if (restoring) R.string.library_restore_failed else R.string.library_export_failed)
}
