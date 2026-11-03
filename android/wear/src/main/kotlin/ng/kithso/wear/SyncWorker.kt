package ng.kithso.wear

import android.content.Context
import android.os.SystemClock
import android.util.Log
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeoutOrNull
import java.util.concurrent.TimeUnit

/**
 * The background sync: about once an hour while the watch is charging on Wi-Fi (the
 * constraints of [schedule]), so a watch on its charger overnight picks up what changed
 * without being opened. It holds sync, asks the core for a session with every member it can
 * reach ([WatchSyncer.syncBackground]), waits until nothing is under way any more, and lets go.
 *
 * It's cheap when nothing is due: the core starts no automatic session with a member it had
 * a complete session with within the last hour (MC_SYNC_AUTO_INTERVAL_MS, read as
 * SyncDevice.autoSyncAfterMs), so a run finds nothing to do unless something changed or an
 * hour has passed, and a run that comes early just opens sync, tries each member's address
 * and stops. The period is an hour too, the shortest the rate limit lets be of use. Only
 * scheduled while the watch is in a group (KithsongApp), and a run outside one ends at once.
 */
class SyncWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result {
        val syncer = (applicationContext as KithsongApp).syncer
        syncer.hold(WatchSyncer.HOLDER_WORKER)
        try {
            val status = withTimeoutOrNull(START_TIMEOUT_MS) { syncer.status.first { it.running || it.error != null } }
            if (status?.running != true) {
                Log.i(TAG, "Background sync: sync didn't start (${status?.error})")
                return Result.success()
            }
            if (!status.inGroup) {
                Log.i(TAG, "Background sync: not in a group")
                return Result.success()
            }
            Log.i(TAG, "Background sync: asking every member")
            syncer.syncBackground()
            // Until nothing has been under way for a while: connections take a moment to be
            // answered, so the first stretch of quiet doesn't count. Bounded, as WorkManager
            // bounds a run anyway; a transfer still going then is cut short and picked up
            // next time.
            val start = SystemClock.elapsedRealtime()
            var quietSince = start
            while (SystemClock.elapsedRealtime() - start < MAX_MS) {
                delay(POLL_MS)
                val now = SystemClock.elapsedRealtime()
                if (syncer.status.value.busy) {
                    quietSince = now
                } else if (now - quietSince >= QUIET_MS && now - start >= SETTLE_MS) {
                    break
                }
            }
            Log.i(TAG, "Background sync: done after ${(SystemClock.elapsedRealtime() - start) / 1000} s")
            return Result.success()
        } finally {
            syncer.release(WatchSyncer.HOLDER_WORKER)
        }
    }

    companion object {
        private const val TAG = "kithsong"
        private const val WORK_NAME = "sync"
        private const val START_TIMEOUT_MS = 15_000L
        private const val SETTLE_MS = 30_000L
        private const val QUIET_MS = 15_000L
        private const val POLL_MS = 2_000L
        // Under WorkManager's ten minutes.
        private const val MAX_MS = 9 * 60_000L

        /** Schedules the hourly run, if it isn't already; it survives restarts and upgrades. */
        fun schedule(context: Context) {
            val constraints = Constraints.Builder()
                .setRequiredNetworkType(NetworkType.UNMETERED)
                .setRequiresCharging(true)
                .build()
            val request = PeriodicWorkRequestBuilder<SyncWorker>(1, TimeUnit.HOURS)
                .setConstraints(constraints)
                .build()
            WorkManager.getInstance(context).enqueueUniquePeriodicWork(WORK_NAME, ExistingPeriodicWorkPolicy.KEEP, request)
        }

        /** Stops the runs: the watch left, or was removed from, its group. */
        fun cancel(context: Context) {
            WorkManager.getInstance(context).cancelUniqueWork(WORK_NAME)
        }
    }
}
