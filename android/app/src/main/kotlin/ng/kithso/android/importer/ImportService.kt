package ng.kithso.android.importer

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
import ng.kithso.android.KithsongApp
import ng.kithso.android.MainActivity
import ng.kithso.android.R
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Keeps the process running while [Importer] copies and imports, as a foreground service of
 * the dataSync type with a progress notification, and holds a wake lock so the import carries
 * on with the screen off. An import of a few hundred albums takes minutes, mostly waiting on
 * MusicBrainz, and the core's sessions live in this process and can't be resumed, so a
 * WorkManager job, which the system may stop and retry, doesn't fit. The notification can
 * resolve the items waiting for a decision and cancel. It stops itself once the import is
 * over, leaving a summary if the app isn't on screen.
 */
class ImportService : Service() {
    private val scope = MainScope()
    private var wakeLock: PowerManager.WakeLock? = null
    private var foreground = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val app = application as KithsongApp
        val importer = app.importer
        when (intent?.action) {
            ACTION_ACCEPT -> importer.acceptAllAsIs()
            ACTION_SKIP -> importer.skipAll()
            ACTION_CANCEL -> importer.cancel()
        }
        if (!foreground) {
            foreground = true
            createChannel(this)
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                notification(importer.progress.value ?: ImportProgress(ImportProgress.Phase.Copying)),
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC else 0,
            )
            wakeLock = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "kithsong:import")
                .apply { acquire(WAKE_LOCK_TIMEOUT_MS) }
            scope.launch {
                importer.progress.collect { progress ->
                    if (progress == null || progress.phase.isOver) {
                        if (progress != null && !app.isVisible) showSummary(progress)
                        stop()
                    } else {
                        NotificationManagerCompat.from(this@ImportService).notifyIfAllowed(NOTIFICATION_ID, notification(progress))
                    }
                }
            }
        }
        return START_NOT_STICKY
    }

    // Android 15 limits data sync services to 6 hours a day.
    override fun onTimeout(startId: Int, fgsType: Int) {
        (application as KithsongApp).importer.cancel()
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

    private fun notification(progress: ImportProgress): android.app.Notification {
        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(progress.title(this))
            .setContentText(progress.detail(this))
            .setContentIntent(openApp())
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .setCategory(NotificationCompat.CATEGORY_PROGRESS)
        when (progress.phase) {
            ImportProgress.Phase.Copying ->
                if (progress.toCopy > 0) builder.setProgress(progress.toCopy, progress.copied, false)
                else builder.setProgress(0, 0, true)
            ImportProgress.Phase.Scanning -> builder.setProgress(0, 0, true)
            ImportProgress.Phase.Matching -> builder.setProgress(progress.items.coerceAtLeast(1), progress.settled, false)
            else -> Unit
        }
        if (progress.phase == ImportProgress.Phase.Review) {
            builder.addAction(0, getString(R.string.import_skip), action(ACTION_SKIP))
            builder.addAction(0, getString(R.string.import_as_tagged), action(ACTION_ACCEPT))
        } else {
            builder.addAction(0, getString(R.string.cancel), action(ACTION_CANCEL))
        }
        return builder.build()
    }

    private fun showSummary(progress: ImportProgress) {
        val notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(progress.title(this))
            .setContentText(progress.detail(this))
            .setContentIntent(openApp())
            .setAutoCancel(true)
            .build()
        NotificationManagerCompat.from(this).notifyIfAllowed(SUMMARY_ID, notification)
    }

    private fun openApp() = PendingIntent.getActivity(
        this,
        0,
        Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )

    private fun action(action: String) = PendingIntent.getService(
        this,
        action.hashCode(),
        Intent(this, ImportService::class.java).setAction(action),
        PendingIntent.FLAG_IMMUTABLE,
    )

    private fun NotificationManagerCompat.notifyIfAllowed(id: Int, notification: android.app.Notification) {
        if (areNotificationsEnabled()) {
            try {
                notify(id, notification)
            } catch (e: SecurityException) {
                Log.w(TAG, "Can't post the import notification", e)
            }
        }
    }

    companion object {
        private const val TAG = "kithsong"
        private const val CHANNEL_ID = "import"
        private const val NOTIFICATION_ID = 2
        private const val SUMMARY_ID = 3
        private const val ACTION_ACCEPT = "ng.kithso.import.ACCEPT"
        private const val ACTION_SKIP = "ng.kithso.import.SKIP"
        private const val ACTION_CANCEL = "ng.kithso.import.CANCEL"
        // A safety net: the service releases it when the import is over.
        private const val WAKE_LOCK_TIMEOUT_MS = 6 * 60 * 60 * 1000L

        /**
         * Starts the service for an import that has just begun. If the system won't let it
         * start (the app is in the background), the import still runs, just without the
         * notification, for as long as the process does.
         */
        fun start(context: Context) {
            try {
                ContextCompat.startForegroundService(context, Intent(context, ImportService::class.java))
            } catch (e: IllegalStateException) {
                // ForegroundServiceStartNotAllowedException, from Android 12, is one of these.
                Log.w(TAG, "Couldn't start the import service", e)
            }
        }

        /** Takes away the summary of the last import, once it has been seen in the app. */
        fun clearSummary(context: Context) = NotificationManagerCompat.from(context).cancel(SUMMARY_ID)

        private fun createChannel(context: Context) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
            val channel = NotificationChannel(CHANNEL_ID, context.getString(R.string.import_channel), NotificationManager.IMPORTANCE_LOW)
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
    }
}

/** A notification's or the import card's headline for [this]. */
fun ImportProgress.title(context: Context): String = when (phase) {
    ImportProgress.Phase.Copying -> context.getString(R.string.import_copying)
    ImportProgress.Phase.Scanning -> context.getString(R.string.import_scanning)
    ImportProgress.Phase.Matching ->
        if (items == 0) context.getString(R.string.import_scanning)
        else context.getString(R.string.import_matching, minOf(settled + 1, items), items)
    ImportProgress.Phase.Review -> context.resources.getQuantityString(R.plurals.import_review, review, review)
    ImportProgress.Phase.Finished -> context.getString(R.string.import_finished)
    ImportProgress.Phase.Cancelled -> context.getString(R.string.import_cancelled)
}

/** The line under [title]. */
fun ImportProgress.detail(context: Context): String {
    val res = context.resources
    val done = res.getQuantityString(R.plurals.import_imported, imported, imported)
    val separator = context.getString(R.string.import_list_separator)
    // Which folder, when there are several.
    val folder = if (batches > 1 && !phase.isOver) context.getString(R.string.import_batch, batch + 1, batches) + separator else ""
    return when (phase) {
        ImportProgress.Phase.Copying ->
            folder + res.getQuantityString(R.plurals.import_copying_files, toCopy, copied, toCopy)
        ImportProgress.Phase.Scanning -> folder + res.getQuantityString(R.plurals.import_files_found, filesFound, filesFound)
        ImportProgress.Phase.Matching ->
            folder + if (review > 0) done + separator + res.getQuantityString(R.plurals.import_need_decision, review, review)
            else done
        ImportProgress.Phase.Review ->
            reviewItems.firstOrNull()?.reason?.let { context.getString(R.string.import_no_match_reason, done, it) }
                ?: context.getString(R.string.import_no_match, done)
        else -> if (items == 0 && phase == ImportProgress.Phase.Finished) context.getString(R.string.import_no_music) else listOfNotNull(
            done,
            res.getQuantityString(R.plurals.import_skipped, skipped, skipped).takeIf { skipped > 0 },
            res.getQuantityString(R.plurals.import_failed, failed, failed).takeIf { failed > 0 },
        ).joinToString(separator)
    }
}
