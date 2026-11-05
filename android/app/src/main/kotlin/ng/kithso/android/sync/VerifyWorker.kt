package ng.kithso.android.sync

import android.content.Context
import android.os.SystemClock
import android.util.Log
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeoutOrNull
import ng.kithso.android.KithsongApp
import ng.kithso.core.MusicCoreException
import java.util.concurrent.TimeUnit

/**
 * The weekly deep check, which reads every file here against its hash: asked for daily while the
 * phone charges ([schedule]), and the core says whether it's due, so most runs just open sync and
 * stop. A desktop runs the check by itself; a phone has no scheduler that may touch the disk for
 * long, so this goes through it in slices. If the system stops it (the charger went, or its time
 * was up) the core remembers how far it got, and the next charge goes on from there.
 *
 * Holding sync starts no sessions, so a run only reads. What it finds shows on the card at the top
 * of the main list, and what failed is fetched at the next session.
 */
class VerifyWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result {
        val syncer = (applicationContext as KithsongApp).syncer
        syncer.hold(Syncer.HOLDER_VERIFY)
        try {
            val status = withTimeoutOrNull(START_TIMEOUT_MS) { syncer.status.first { it.running || it.error != null } }
            if (status?.running != true) {
                Log.i(TAG, "Weekly check: sync didn't start (${status?.error})")
                return Result.retry()
            }
            val start = SystemClock.elapsedRealtime()
            while (SystemClock.elapsedRealtime() - start < MAX_MS) {
                if (syncer.verifyWeekly(SLICE_MS)) {
                    Log.i(TAG, "Weekly check: nothing more this week")
                    return Result.success()
                }
            }
            Log.i(TAG, "Weekly check: out of time, the rest next time")
            return Result.retry()
        } catch (e: MusicCoreException) {
            Log.w(TAG, "Weekly check failed", e)
            return Result.retry()
        } finally {
            syncer.release(Syncer.HOLDER_VERIFY)
        }
    }

    companion object {
        private const val TAG = "kithsong"
        private const val WORK_NAME = "verify"
        private const val START_TIMEOUT_MS = 15_000L
        // Short enough that a run told to stop isn't kept long.
        private const val SLICE_MS = 10_000L
        // Under WorkManager's ten minutes, so a long run ends as a retry rather than being cut off.
        private const val MAX_MS = 9 * 60_000L

        /** Schedules the daily run, if it isn't already; it survives restarts and upgrades. */
        fun schedule(context: Context) {
            val request = PeriodicWorkRequestBuilder<VerifyWorker>(1, TimeUnit.DAYS)
                .setConstraints(Constraints.Builder().setRequiresCharging(true).build())
                .build()
            WorkManager.getInstance(context).enqueueUniquePeriodicWork(WORK_NAME, ExistingPeriodicWorkPolicy.KEEP, request)
        }
    }
}
