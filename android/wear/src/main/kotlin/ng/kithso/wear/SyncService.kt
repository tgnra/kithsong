package ng.kithso.wear

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
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

/**
 * Keeps the process, and so sync, running while a session or a pairing is under way, as a
 * foreground service of the dataSync type with a wake lock, so leaving the app or the screen
 * going dark doesn't cut it off halfway. It stops itself once [WatchSyncer] is idle. The
 * phone's SyncService, trimmed: the watch's notification is just a title and, while files
 * come in, how many.
 *
 * It only ever finishes what began on screen: it's started as a session the app shows or a
 * pairing begins, which happens only while the app is on screen, and once the app has left,
 * [WatchSyncer] has the core start nothing new by itself. Sessions off screen are the
 * background job's ([SyncWorker]), which holds sync itself and needs no service.
 */
class SyncService : Service() {
    private val scope = MainScope()
    private var wakeLock: PowerManager.WakeLock? = null
    private var foreground = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val syncer = (application as KithsongApp).syncer
        if (!foreground) {
            foreground = true
            createChannel(this)
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                notification(syncer.status.value),
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC,
            )
            val lock = getSystemService(PowerManager::class.java)
                .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "kithsong:sync")
                .apply { setReferenceCounted(false) }
            wakeLock = lock
            syncer.hold(HOLDER)
            scope.launch {
                syncer.status.collect { status ->
                    if (!status.busy) {
                        stop()
                    } else {
                        // Renewed as sync goes on, so a long file transfer keeps it.
                        lock.acquire(WAKE_LOCK_TIMEOUT_MS)
                        NotificationManagerCompat.from(this@SyncService).notifyIfAllowed(notification(status))
                    }
                }
            }
        }
        return START_NOT_STICKY
    }

    // Android 15 limits data sync services to 6 hours a day.
    override fun onTimeout(startId: Int, fgsType: Int) {
        (application as KithsongApp).syncer.cancelPairing()
        stop()
    }

    override fun onDestroy() {
        if (foreground) (application as KithsongApp).syncer.release(HOLDER)
        wakeLock?.takeIf { it.isHeld }?.release()
        scope.cancel()
        super.onDestroy()
    }

    private fun stop() {
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun notification(status: SyncStatus): Notification {
        val pairing = status.pairing is Pairing.ShowCode
        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(getString(if (pairing) R.string.pairing else R.string.syncing))
            .setContentIntent(openApp())
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .setCategory(NotificationCompat.CATEGORY_PROGRESS)
        val files = status.filesProgress
        if (pairing) {
            builder.setContentText(getString(R.string.watch_keep_open))
        } else if (files != null) {
            builder.setContentText(resources.getQuantityString(R.plurals.sync_notification_files, files.total, files.done, files.total))
            builder.setProgress(files.total, files.done, false)
        } else {
            builder.setProgress(0, 0, true)
        }
        return builder.build()
    }

    private fun openApp() = PendingIntent.getActivity(
        this,
        0,
        Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )

    private fun NotificationManagerCompat.notifyIfAllowed(notification: Notification) {
        if (areNotificationsEnabled()) {
            try {
                notify(NOTIFICATION_ID, notification)
            } catch (e: SecurityException) {
                Log.w(TAG, "Can't post the sync notification", e)
            }
        }
    }

    companion object {
        private const val TAG = "kithsong"
        private const val HOLDER = "service"
        private const val CHANNEL_ID = "sync"
        private const val NOTIFICATION_ID = 4
        // A safety net, if sync goes this long without a step forward: the service releases it
        // once sync is idle.
        private const val WAKE_LOCK_TIMEOUT_MS = 30 * 60 * 1000L

        /**
         * Starts the service as a session or pairing begins, while the app is on screen. If the
         * system won't let it start even so (the app was just leaving), nothing holds sync once
         * the app has gone, and it stops there, cutting off what was under way.
         */
        fun start(context: Context) {
            try {
                ContextCompat.startForegroundService(context, Intent(context, SyncService::class.java))
            } catch (e: IllegalStateException) {
                // ForegroundServiceStartNotAllowedException, from Android 12, is one of these.
                Log.w(TAG, "Couldn't start the sync service", e)
            }
        }

        private fun createChannel(context: Context) {
            val channel = NotificationChannel(CHANNEL_ID, context.getString(R.string.sync), NotificationManager.IMPORTANCE_LOW)
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
    }
}
