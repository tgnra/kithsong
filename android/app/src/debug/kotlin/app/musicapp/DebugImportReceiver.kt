package app.musicapp

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import app.musicapp.importer.ImportProgress
import kotlinx.coroutines.DelicateCoroutinesApi
import kotlinx.coroutines.GlobalScope
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

/**
 * Debug builds only, for loading test data without the UI: imports a folder through the app's
 * [app.musicapp.importer.Importer], as the import button does (progress shows in the app),
 * and imports every item that needs a decision as tagged.
 *
 * ```
 * adb push music /sdcard/Android/data/app.musicapp/files/import
 * adb shell am broadcast -n app.musicapp/.DebugImportReceiver [--es path <folder>] [--ez art true]
 * ```
 *
 * The folder defaults to the app's external files folder `import`, which needs no permission.
 * Covers aren't downloaded unless `art` is true. Each item is looked up on MusicBrainz first, a
 * request a second, so turn the network off to import made-up music without wrong matches.
 * The result goes to logcat under `musicapp`.
 */
class DebugImportReceiver : BroadcastReceiver() {
    @OptIn(DelicateCoroutinesApi::class)
    override fun onReceive(context: Context, intent: Intent) {
        val importer = (context.applicationContext as MusicApp).importer
        val path = intent.getStringExtra("path") ?: context.getExternalFilesDir("import")?.path ?: return
        val start = System.currentTimeMillis()
        if (!importer.start(listOf(path), fetchArt = intent.getBooleanExtra("art", false))) {
            Log.w(TAG, "Debug import: an import is already running")
            return
        }
        // It takes longer than a receiver may run, so it carries on in the process.
        GlobalScope.launch {
            val progress = importer.progress.filterNotNull().first {
                it.phase == ImportProgress.Phase.Review || it.phase.isOver
            }
            if (progress.phase == ImportProgress.Phase.Review) importer.acceptAllAsIs()
            val end = importer.progress.filterNotNull().first { it.phase.isOver }
            Log.i(
                TAG,
                "Debug import of $path finished in ${System.currentTimeMillis() - start} ms: " +
                    "${end.imported} imported, ${end.skipped} skipped, ${end.failed} failed",
            )
        }
    }

    private companion object {
        const val TAG = "musicapp"
    }
}
