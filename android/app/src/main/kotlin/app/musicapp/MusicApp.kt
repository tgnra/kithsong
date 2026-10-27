package app.musicapp

import android.app.Application
import android.content.Context
import android.os.Build
import android.provider.Settings
import android.util.Log
import app.musicapp.data.CollectionRef
import app.musicapp.data.CoreLibrarySource
import app.musicapp.data.FakeLibrary
import app.musicapp.data.LibrarySource
import app.musicapp.importer.Importer
import app.musicapp.playback.FakePlayback
import app.musicapp.playback.Playback
import app.musicapp.playback.PlaybackService
import app.musicapp.playback.SessionPlayback
import app.musicapp.sync.Syncer
import app.musicapp.ui.art.FileArt
import app.musicapp.ui.art.artImageLoader
import coil3.ImageLoader
import coil3.SingletonImageLoader
import java.io.File
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.runBlocking

/**
 * Holds the library, the player, the importer and sync for the life of the process, so they
 * outlive the activity. All come from the core; the player plays in [PlaybackService], and
 * [playback] is the UI's connection to it. Imports run in the background too ([Importer]), and
 * sync runs while the app is on screen and now and then in the background ([Syncer]).
 */
class MusicApp : Application(), SingletonImageLoader.Factory {
    /**
     * The library folder: library.db and the music, which import copies in. It's in the app's
     * internal no-backup storage, never on an SD card: the music is far too big for Auto
     * Backup, and a library.db restored onto a new phone would carry this phone's sync node id
     * with it.
     */
    val libraryDir: File get() = File(noBackupFilesDir, "library")

    lateinit var library: LibrarySource
        private set
    lateinit var playback: Playback
        private set
    lateinit var settings: AppSettings
        private set

    /** Imports into the core's library, even while the made-up one is shown. */
    lateinit var importer: Importer
        private set

    /** Syncs the core's library with this phone's group, even while the made-up one is shown. */
    lateinit var syncer: Syncer
        private set

    /** Whether the activity is on screen; set by [MainActivity]. Sync runs while it is. */
    var isVisible = false
        set(value) {
            if (value == field) return
            field = value
            if (value) syncer.hold(Syncer.HOLDER_APP) else syncer.release(Syncer.HOLDER_APP)
        }

    override fun onCreate() {
        super.onCreate()
        val status = NativeCore.init()
        if (status != 0) Log.e(TAG, "musiccore init failed: $status")
        val core = CoreLibrarySource(
            libraryDir,
            getSharedPreferences("library", MODE_PRIVATE),
            MainScope(),
            getString(R.string.unknown_artist),
        )
        library = core
        playback = SessionPlayback(this, MainScope())
        settings = AppSettings(getSharedPreferences("app", MODE_PRIVATE))
        importer = Importer(this, core, MainScope())
        // sync.db holds this phone's keys, so it's in no-backup storage like the library: a
        // restored copy on another phone would pose as this one.
        syncer = Syncer(this, core, File(noBackupFilesDir, "sync"), deviceName(), MainScope())
        core.onChanged = syncer::notifyChanged
    }

    // What other devices call this phone: the name set in the system settings (API 25+), else
    // its model.
    private fun deviceName(): String {
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
        const val TAG = "musicapp"
    }
}
