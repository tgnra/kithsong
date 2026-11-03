package ng.kithso.wear

import android.app.Application
import android.content.res.Configuration
import android.os.Build
import android.provider.Settings
import android.util.Log
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.filter
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import ng.kithso.core.CoreLibrary
import ng.kithso.core.NativeCore
import ng.kithso.wear.playback.AudioOutputs
import ng.kithso.wear.playback.Scrobbling
import ng.kithso.wear.playback.SessionPlayback
import java.io.File

/**
 * Holds the library, sync, the UI's connection to the player and what goes with playing for
 * the life of the process. The player itself runs in [ng.kithso.wear.playback.PlaybackService],
 * so music keeps playing after the activity closes. Sync runs while the app is on screen,
 * while what it was doing then finishes ([SyncService]) and, while the watch is in a group,
 * about once an hour on the charger ([SyncWorker]).
 */
class KithsongApp : Application() {
    /**
     * The library folder: library.db and the music, which sync brings in. In the app's
     * no-backup storage, as on the phone: the music is far too big for Auto Backup, and a
     * library.db restored onto another watch would carry this one's sync node id with it.
     */
    val libraryDir: File get() = File(noBackupFilesDir, "library")

    /** What has files on the watch, and the library's reading thread. */
    lateinit var library: WatchLibrary
        private set
    lateinit var syncer: WatchSyncer
        private set

    /** What is playing, and the commands, through the playback service's session. */
    val playback: SessionPlayback by lazy { SessionPlayback(this, MainScope()) }

    /**
     * Counts plays and passes them on to the group's Last.fm account. On a library handle of
     * its own, used on the main thread: [library]'s lives on its reading thread, and a handle
     * is used by one thread at a time.
     */
    val scrobbling: Scrobbling by lazy { Scrobbling(CoreLibrary(libraryDir.path), File(noBackupFilesDir, "sync")) }

    /** Headphones or the watch's speaker, settled before playing. */
    val outputs: AudioOutputs by lazy { AudioOutputs(this) }

    private val prefs by lazy { getSharedPreferences("app", MODE_PRIVATE) }

    // The core's network stack, for sync and scrobbling, and its own text (errors and the like)
    // in the app's language.
    override fun onCreate() {
        super.onCreate()
        val status = NativeCore.init()
        if (status != 0) Log.e(TAG, "musiccore init failed: $status")
        NativeCore.followLocale(resources.configuration)
        moveOldLibrary()
        val scope = MainScope()
        library = WatchLibrary(libraryDir, scope)
        // sync.db holds this watch's keys, so it's in no-backup storage like the library: a
        // restored copy on another watch would pose as this one. A name the system's isn't
        // saved, so the watch follows the system's while it hasn't been renamed.
        val name = prefs.getString(KEY_DEVICE_NAME, null) ?: systemDeviceName()
        syncer = WatchSyncer(this, library, File(noBackupFilesDir, "sync"), name, scope) { renamed ->
            prefs.edit().putString(KEY_DEVICE_NAME, renamed.takeIf { it != systemDeviceName() }).apply()
        }
        library.onChanged = syncer::notifyChanged
        // On screen (the process lifecycle waits a moment before saying it's gone): sync runs
        // and starts sessions by itself, as on the phone.
        ProcessLifecycleOwner.get().lifecycle.addObserver(object : DefaultLifecycleObserver {
            override fun onStart(owner: LifecycleOwner) = syncer.hold(WatchSyncer.HOLDER_APP)

            override fun onStop(owner: LifecycleOwner) = syncer.release(WatchSyncer.HOLDER_APP)
        })
        // The hourly background sync runs only while the watch is in a group, which is known
        // once sync has started.
        scope.launch {
            syncer.status.filter { it.running }.map { it.inGroup }.distinctUntilChanged().collect { inGroup ->
                if (inGroup) SyncWorker.schedule(this@KithsongApp) else SyncWorker.cancel(this@KithsongApp)
            }
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        NativeCore.followLocale(newConfig)
    }

    // What other devices call this watch unless it's renamed: the name set in the system
    // settings, else its model.
    private fun systemDeviceName(): String {
        val name = Settings.Global.getString(contentResolver, Settings.Global.DEVICE_NAME)
        return name?.takeIf { it.isNotBlank() } ?: Build.MODEL
    }

    // The prototype kept the library in filesDir, where Auto Backup would take it: moved once.
    private fun moveOldLibrary() {
        val old = File(filesDir, "library")
        if (!old.isDirectory || libraryDir.exists()) return
        noBackupFilesDir.mkdirs()
        if (old.renameTo(libraryDir)) {
            Log.i(TAG, "Moved the library to no-backup storage")
        } else {
            Log.w(TAG, "Couldn't move the old library at $old; starting afresh")
        }
    }

    private companion object {
        const val TAG = "kithsong"
        const val KEY_DEVICE_NAME = "device_name"
    }
}
