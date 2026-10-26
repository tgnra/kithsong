package app.musicapp.wear

import android.app.Application
import app.musicapp.CoreLibrary
import java.io.File

/**
 * Holds the library and the player for the life of the process, so music keeps playing
 * after the activity closes.
 */
class MusicApp : Application() {
    /** The library folder, filled by sync; library.db lives inside it. */
    val library: CoreLibrary by lazy { CoreLibrary(File(filesDir, "library").absolutePath) }
    val player: Player by lazy { Player() }
}
