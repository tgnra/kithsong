package ng.kithso.android.sync

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
import ng.kithso.android.MainActivity
import ng.kithso.android.KithsongApp
import ng.kithso.android.R
import ng.kithso.core.CoreSync
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch

/**
 * Keeps the process, and so sync, running while a session or a pairing is under way, as a
 * foreground service of the dataSync type with a wake lock, so leaving the app or turning the
 * screen off doesn't cut it off halfway. It stops itself once [Syncer] is idle. Most sessions
 * take a second or two, and Android 12+ holds back a foreground service's notification for
 * the first few seconds, so it's seldom seen.
 */
class SyncService : Service() {
    private val scope = MainScope()
    private var wakeLock: PowerManager.WakeLock? = null
    private var foreground = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val syncer = (application as KithsongApp).syncer
        if (intent?.action == ACTION_CANCEL) syncer.cancelPairing()
        if (intent?.action == ACTION_CANCEL_SYNC) syncer.cancelSync()
        if (!foreground) {
            foreground = true
            createChannel(this)
            ServiceCompat.startForeground(
                this,
                NOTIFICATION_ID,
                notification(syncer.status.value),
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC else 0,
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
        val pairing = status.pairing
        val names = status.members.filter { it.uid in status.syncing }.map { it.name }
        // The merge after pairing by merging libraries, while no session or pairing is shown.
        val merge = status.merge?.takeIf { it.running && names.isEmpty() && (pairing == null || pairing is Pairing.Ended) }
        val title = when (pairing) {
            is Pairing.EnterCode -> getString(R.string.sync_notification_pairing_with, pairing.peer)
            is Pairing.ShowCode -> getString(R.string.sync_notification_wants_to_pair, pairing.peer)
            else -> if (merge != null) getString(R.string.merge_title)
            else if (names.isEmpty()) getString(R.string.syncing)
            else getString(R.string.sync_notification_syncing_with, names.joinToString())
        }
        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(title)
            .setContentIntent(openApp())
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .setCategory(NotificationCompat.CATEGORY_PROGRESS)
        if (pairing is Pairing.ShowCode) builder.setContentText(getString(R.string.sync_notification_see_code))
        // Files being fetched or sent, in every session at once; usually there's just one.
        val files = status.sessions.values.filter { it.moving > 0 }
        val filesDone = files.sumOf { it.filesDone }
        val filesTotal = files.sumOf { it.filesTotal }
        val moved = files.sumOf { it.moved }
        val moving = files.sumOf { it.moving }
        if (pairing is Pairing.EnterCode || pairing is Pairing.ShowCode) {
            builder.addAction(0, getString(R.string.cancel), cancel(ACTION_CANCEL))
        } else if (merge != null) {
            val phase = if (merge.phase == CoreSync.MergePhase.Tracks) R.string.merge_progress_tracks else R.string.merge_progress_lists
            builder.setContentText(getString(phase, merge.done, merge.total))
            builder.setProgress(MERGE_STEPS, (merge.fraction * MERGE_STEPS).toInt(), false)
        } else {
            if (moving > 0) {
                val text = if (files.all { it.sending }) {
                    val sendsDone = files.sumOf { it.sendsDone }
                    val sendsTotal = files.sumOf { it.sendsTotal }
                    resources.getQuantityString(R.plurals.sync_notification_files_sent, sendsTotal, sendsDone, sendsTotal)
                } else {
                    resources.getQuantityString(R.plurals.sync_notification_files, filesTotal, filesDone, filesTotal)
                }
                builder.setContentText(text)
                builder.setProgress(moving, moved, false)
            } else {
                builder.setProgress(0, 0, true)
            }
            if (status.syncing.isNotEmpty()) builder.addAction(0, getString(R.string.cancel_sync), cancel(ACTION_CANCEL_SYNC))
        }
        return builder.build()
    }

    private fun openApp() = PendingIntent.getActivity(
        this,
        0,
        Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )

    // Distinct request codes, so the two actions' intents don't stand in for each other.
    private fun cancel(action: String) = PendingIntent.getService(
        this,
        if (action == ACTION_CANCEL_SYNC) 1 else 0,
        Intent(this, SyncService::class.java).setAction(action),
        PendingIntent.FLAG_IMMUTABLE,
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
        private const val MERGE_STEPS = 1000
        private const val ACTION_CANCEL = "ng.kithso.android.sync.CANCEL"
        private const val ACTION_CANCEL_SYNC = "ng.kithso.android.sync.CANCEL_SYNC"
        // A safety net, if sync goes this long without a step forward: the service releases it
        // once sync is idle.
        private const val WAKE_LOCK_TIMEOUT_MS = 30 * 60 * 1000L

        /**
         * Starts the service as a session or pairing begins. If the system won't let it start
         * (the app is in the background), sync carries on while the process lives.
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
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
            val channel = NotificationChannel(CHANNEL_ID, context.getString(R.string.sync), NotificationManager.IMPORTANCE_LOW)
            context.getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
    }
}
