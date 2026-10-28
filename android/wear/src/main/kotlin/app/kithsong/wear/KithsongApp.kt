package app.kithsong.wear

import android.app.Application
import android.content.res.Configuration
import app.kithsong.CoreLibrary
import app.kithsong.NativeCore
import java.io.File

/**
 * Holds the library and the player for the life of the process, so music keeps playing
 * after the activity closes.
 */
class KithsongApp : Application() {
    /** The library folder, filled by sync; library.db lives inside it. */
    val library: CoreLibrary by lazy { CoreLibrary(File(filesDir, "library").absolutePath) }
    val player: Player by lazy { Player() }

    // The core's own text (errors and the like) in the app's language.
    override fun onCreate() {
        super.onCreate()
        NativeCore.followLocale(resources.configuration)
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        NativeCore.followLocale(newConfig)
    }
}
