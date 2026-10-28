package app.musicapp.sync

import android.content.Context
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import app.musicapp.MusicApp
import java.util.concurrent.TimeUnit

/**
 * Syncs with the group now and then while the app isn't on screen, on an unmetered network
 * (Wi-Fi or Ethernet, where the other devices are), so plays and edits made elsewhere are here
 * before the app is opened, and this phone's reach the others. A session of library data takes
 * seconds, but audio files may take much longer, so each run waits for nearly the ten minutes
 * a worker gets. A transfer cut off then keeps the files that arrived whole, and the next run
 * carries on with the rest. Without the app on screen there's no foreground service: Android
 * won't start one from the background. A member synced completely within the last hour, from
 * here or by itself, gets no session until the hour is up (see [Syncer.syncOnce]).
 */
class SyncWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    override suspend fun doWork(): Result {
        val synced = (applicationContext as MusicApp).syncer.syncOnce(TIMEOUT_MS)
        return if (synced) Result.success() else Result.retry()
    }

    companion object {
        private const val NAME = "sync"
        // A worker is stopped after 10 minutes; this leaves time to stop sync cleanly.
        private const val TIMEOUT_MS = 9 * 60 * 1000L

        /** Syncs every hour or so while this phone is in a group, [inGroup]; else not at all. */
        fun schedule(context: Context, inGroup: Boolean) {
            val work = WorkManager.getInstance(context)
            if (!inGroup) {
                work.cancelUniqueWork(NAME)
                return
            }
            val request = PeriodicWorkRequestBuilder<SyncWorker>(1, TimeUnit.HOURS)
                .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.UNMETERED).build())
                .build()
            work.enqueueUniquePeriodicWork(NAME, ExistingPeriodicWorkPolicy.KEEP, request)
        }
    }
}
