package ng.kithso.android.importer

import android.content.Context
import android.os.Build
import android.os.SystemClock
import android.util.Log
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingWorkPolicy
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.withContext
import ng.kithso.android.KithsongApp
import ng.kithso.android.data.CoreLibrarySource
import ng.kithso.core.CoreAnalysis
import ng.kithso.core.MusicCoreException

/**
 * Analyses what an import brought, the next time the phone is charging. A phone doesn't scan by
 * itself, for the battery: results come by sync from a desktop that holds the music. But music
 * imported here may have no such desktop, so an import that added tracks asks for one run of
 * this ([schedule]), and so does Settings' Analyse, for what an import didn't leave a run for.
 * Nothing else does: there's no periodic work and no scan at start.
 *
 * It has no list of tracks. The core works through those with a file here and no valid result,
 * so whatever sync delivered in the meantime is skipped, and it stands aside by itself during
 * sync sessions and imports. If the system stops it (the charger went, or its time was up) the
 * results so far stay, and WorkManager runs it again on the next charge, when it goes on from
 * there. Having been through the tracks it's over, and nothing is scheduled. However it ends,
 * the Radio row looks again at what there is to go on.
 */
class AnalysisWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result {
        val app = applicationContext as KithsongApp
        val analysis = try {
            CoreAnalysis(app.libraryDir.path)
        } catch (e: MusicCoreException) {
            Log.w(TAG, "Analysis: couldn't start", e)
            return Result.retry()
        }
        Log.i(TAG, "Analysis: started")
        try {
            val start = SystemClock.elapsedRealtime()
            while (SystemClock.elapsedRealtime() - start < MAX_MS) {
                when (analysis.state) {
                    CoreAnalysis.State.Finished -> {
                        Log.i(TAG, "Analysis: finished")
                        return Result.success()
                    }
                    CoreAnalysis.State.GaveUp -> {
                        Log.w(TAG, "Analysis: gave up for now")
                        return Result.retry()
                    }
                    CoreAnalysis.State.Running -> delay(POLL_MS)
                }
            }
            Log.i(TAG, "Analysis: out of time, the rest next time")
            return Result.retry()
        } catch (e: CancellationException) {
            val reason = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) " ($stopReason)" else ""
            Log.i(TAG, "Analysis: stopped by the system$reason")
            throw e
        } finally {
            withContext(NonCancellable + Dispatchers.IO) { analysis.stop() }
            (app.library as? CoreLibrarySource)?.refreshStations()
        }
    }

    companion object {
        private const val TAG = "kithsong"
        private const val WORK_NAME = "analysis"
        private const val POLL_MS = 1_000L
        // Under WorkManager's ten minutes, so a long run ends as a retry rather than being cut off.
        private const val MAX_MS = 9 * 60_000L

        /** Whether a run is waiting for the charger or under way, as that changes. */
        fun asked(context: Context): Flow<Boolean> =
            WorkManager.getInstance(context).getWorkInfosForUniqueWorkFlow(WORK_NAME)
                .map { runs -> runs.any { !it.state.isFinished } }
                .distinctUntilChanged()

        /**
         * Asks for a run while charging. REPLACE, because the job has no list to lose: a run
         * waiting to start is as good as this one, and one under way may already have passed
         * where the new tracks are, so it's stopped, keeping its results, for a run that looks
         * at everything. KEEP would leave those tracks until the next import.
         */
        fun schedule(context: Context) {
            val request = OneTimeWorkRequestBuilder<AnalysisWorker>()
                .setConstraints(Constraints.Builder().setRequiresCharging(true).build())
                .build()
            WorkManager.getInstance(context).enqueueUniqueWork(WORK_NAME, ExistingWorkPolicy.REPLACE, request)
        }
    }
}
