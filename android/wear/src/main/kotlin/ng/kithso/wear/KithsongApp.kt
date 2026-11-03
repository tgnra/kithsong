package ng.kithso.wear

import android.app.Application
import android.content.res.Configuration
import android.util.Log
import kotlinx.coroutines.MainScope
import ng.kithso.core.CoreLibrary
import ng.kithso.core.NativeCore
import ng.kithso.wear.playback.AudioOutputs
import ng.kithso.wear.playback.Scrobbling
import ng.kithso.wear.playback.SessionPlayback
import java.io.File

/**
 * Holds the library, the UI's connection to the player and what goes with playing for the
 * life of the process. The player itself runs in [ng.kithso.wear.playback.PlaybackService], so
 * music keeps playing after the activity closes.
 */
class KithsongApp : Application() {
    /** The library folder, filled by sync; library.db lives inside it. */
    val library: CoreLibrary by lazy { CoreLibrary(File(filesDir, "library").absolutePath) }

    /** What is playing, and the commands, through the playback service's session. */
    val playback: SessionPlayback by lazy { SessionPlayback(this, MainScope()) }

    /** Counts plays and passes them on to the group's Last.fm account. */
    val scrobbling: Scrobbling by lazy { Scrobbling(library, File(noBackupFilesDir, "sync")) }

    /** Headphones or the watch's speaker, settled before playing. */
    val outputs: AudioOutputs by lazy { AudioOutputs(this) }

    // The core's network stack, for scrobbling, and its own text (errors and the like) in the
    // app's language.
    override fun onCreate() {
        super.onCreate()
        val status = NativeCore.init()
        if (status != 0) Log.e("kithsong", "musiccore init failed: $status")
        NativeCore.followLocale(resources.configuration)
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        NativeCore.followLocale(newConfig)
    }
}
