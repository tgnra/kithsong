package ng.kithso.android

import ng.kithso.core.*
import android.app.Application
import android.content.Context
import android.content.res.Configuration
import android.os.Build
import android.provider.Settings
import android.util.Log
import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import androidx.work.WorkManager
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.CoreLibrarySource
import ng.kithso.android.data.FakeLibrary
import ng.kithso.android.data.LibrarySource
import ng.kithso.android.importer.Importer
import ng.kithso.android.playback.FakePlayback
import ng.kithso.android.playback.Playback
import ng.kithso.android.playback.PlaybackService
import ng.kithso.android.playback.Scrobbling
import ng.kithso.android.playback.SessionPlayback
import ng.kithso.android.sync.Syncer
import ng.kithso.android.sync.VerifyWorker
import ng.kithso.android.ui.BackStack
import ng.kithso.android.ui.art.FileArt
import ng.kithso.android.ui.art.artImageLoader
import coil3.ImageLoader
import coil3.SingletonImageLoader
import java.io.File
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.runBlocking

/**
 * Holds the library, the player, the importer and sync for the life of the process, so they
 * outlive the activity. All come from the core; the player plays in [PlaybackService], and
 * [playback] is the UI's connection to it. Imports run in the background too ([Importer]); sync
 * runs only while the app is on screen, and while what it was doing then finishes ([Syncer]).
 */
class KithsongApp : Application(), SingletonImageLoader.Factory {
    /**
     * The library folder: library.db and the music, which sync brings in. It's in the app's
     * internal no-backup storage, never on an SD card: the music is far too big for Auto
     * Backup, and a library.db restored onto a new phone would carry this phone's sync node id
     * with it.
     */
    val realLibraryDir: File get() = File(noBackupFilesDir, "library")

    /**
     * The library this process runs on: [realLibraryDir], or a stress test library if one was
     * opened ([Testing]). Only ever a folder the generator finished, so a stray setting can't
     * point the app anywhere else. It stays for the life of the process.
     */
    val libraryDir: File get() = stressDir ?: realLibraryDir
    private var stressDir: File? = null

    /** Settings' Testing section. */
    lateinit var testing: Testing
        private set

    /** Playlists' and album groups' pictures, which the core makes again if the system clears them away. */
    val playlistArtDir: File get() = File(cacheDir, PLAYLIST_ART_DIR)

    lateinit var library: LibrarySource
        private set
    lateinit var playback: Playback
        private set
    lateinit var settings: AppSettings
        private set

    /** Passes plays on to Last.fm for the group's account (see [Scrobbling]). */
    lateinit var scrobbling: Scrobbling
        private set

    /** Imports into the core's library, even while the made-up one is shown. */
    lateinit var importer: Importer
        private set

    /** Syncs the core's library with this phone's group, even while the made-up one is shown. */
    lateinit var syncer: Syncer
        private set

    /**
     * Whether the app is on screen, as the process lifecycle has it (which waits a moment
     * before saying it's gone, so turning the phone doesn't count). Sync runs while it is.
     */
    var isVisible = false
        private set(value) {
            if (value == field) return
            field = value
            if (value) syncer.hold(Syncer.HOLDER_APP) else syncer.release(Syncer.HOLDER_APP)
        }

    override fun onCreate() {
        super.onCreate()
        val startedUs = CoreTesting.nowUs()
        val status = NativeCore.init()
        if (status != 0) Log.e(TAG, "kithcore init failed: $status")
        NativeCore.followLocale(resources.configuration)
        // settings.json, as the desktops have, with the app's own preferences in it as well.
        settings = AppSettings(CoreSettings(filesDir.path), getSharedPreferences("app", MODE_PRIVATE))
        // First thing, for startup to be measured; the start itself comes from the system.
        CoreTesting.setEnabled(settings.perfProfile)
        CoreTesting.markAt("main_entered", startedUs)
        CoreTesting.mark("settings_loaded")
        // The click: App ends the view switch once the page is drawn.
        // By toString(), as R8 renames classes but not what a data class says it's called.
        BackStack.onPush = { page ->
            if (CoreTesting.enabled()) CoreTesting.viewBegin(page.toString().substringBefore('('))
        }
        stressDir =settings.stressLibrary?.takeIf(CoreTesting::isComplete)?.let(::File)
        val core = CoreLibrarySource(
            libraryDir,
            playlistArtDir,
            File(cacheDir, COVER_DRAFT_DIR),
            // A stress library's pictures and such aren't the real one's to remember.
            getSharedPreferences(if (stressDir == null) "library" else "library-stress", MODE_PRIVATE),
            MainScope(),
        ) { getString(R.string.unknown_artist) }
        library = core
        playback = SessionPlayback(this, MainScope())
        // sync.db holds this phone's keys, so it's in no-backup storage like the library: a
        // restored copy on another phone would pose as this one. A name the system's isn't
        // saved, so the phone follows the system's while it hasn't been renamed.
        val name = settings.deviceName ?: systemDeviceName()
        syncer = Syncer(this, core, File(noBackupFilesDir, "sync"), name, MainScope()) { renamed ->
            settings.deviceName = renamed.takeIf { it != systemDeviceName() }
        }
        core.onChanged = syncer::notifyChanged
        core.binOffered = syncer::binOffered
        VerifyWorker.schedule(this)
        importer = Importer(this, core, MainScope())
        scrobbling = Scrobbling(libraryDir, File(noBackupFilesDir, "sync"), settings, MainScope())
        testing = Testing(this, noBackupFilesDir, settings, MainScope())
        CoreTesting.mark("app_created")
        ProcessLifecycleOwner.get().lifecycle.addObserver(object : DefaultLifecycleObserver {
            override fun onStart(owner: LifecycleOwner) {
                isVisible = true
                (library as? CoreLibrarySource)?.onShown()
            }

            override fun onStop(owner: LifecycleOwner) {
                isVisible = false
            }
        })
        // Up to 0.4.2 a WorkManager job synced every hour or so in the background. Sync runs
        // only on screen now, so a job an older version scheduled is cancelled. It's a no-op
        // once gone; it can go when nobody upgrades from 0.4.2 or older any more.
        WorkManager.getInstance(this).cancelUniqueWork(OLD_SYNC_WORK)
    }

    // The system language changed: the core's text follows it, and the library is read again
    // for the "Unknown artist" in it.
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        if (NativeCore.followLocale(newConfig)) (library as? CoreLibrarySource)?.refresh()
    }

    // What other devices call this phone unless it's renamed: the name set in the system
    // settings (API 25+), else its model.
    private fun systemDeviceName(): String {
        val name = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
            Settings.Global.getString(contentResolver, Settings.Global.DEVICE_NAME)
        } else {
            null
        }
        return name?.takeIf { it.isNotBlank() } ?: Build.MODEL
    }

    /**
     * Swaps in the made-up library, empty if [empty], and a pretend player, to try the UI
     * without music.
     */
    fun useFakeLibrary(empty: Boolean) {
        library = FakeLibrary(empty)
        playback = FakePlayback(MainScope()).also(::loadFirstAlbum)
    }

    // Puts something in the mini player, as if restored from last time. The fake library
    // answers at once, so blocking here is fine.
    private fun loadFirstAlbum(playback: FakePlayback) {
        val album = library.library.value?.albums?.firstOrNull()
        val list = album?.let { runBlocking { library.trackList(CollectionRef(CollectionRef.Kind.Album, it.uid)) } }
        playback.load(list?.tracks.orEmpty(), 2)
    }

    override fun newImageLoader(context: Context): ImageLoader = artImageLoader(context, FileArt())

    private companion object {
        const val TAG = "kithsong"
        const val PLAYLIST_ART_DIR = "playlist-art" // MC_PLAYLIST_ART_CACHE_DIR
        const val COVER_DRAFT_DIR = "cover-draft" // a cover chosen in the tag editor, not saved yet
        const val OLD_SYNC_WORK = "sync" // the unique work name SyncWorker was scheduled under
    }
}
