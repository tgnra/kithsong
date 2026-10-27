package app.musicapp.importer

import android.Manifest
import android.content.Context
import android.os.Build
import android.os.Environment
import android.os.storage.StorageManager
import android.provider.MediaStore
import android.util.Log
import androidx.compose.runtime.Immutable
import app.musicapp.R
import java.io.File

/**
 * A folder of music in shared storage, offered for import: a top-level folder of a storage
 * volume, such as Music or Download, with the music files MediaStore knows of under it.
 * [volume] is the volume's name, e.g. "Internal storage" or an SD card's.
 */
@Immutable
data class MusicFolder(val path: String, val name: String, val volume: String, val files: List<String>)

/**
 * The music on the phone's shared storage, as MediaStore indexes it. Reading it takes
 * [AUDIO_PERMISSION]. The core takes real paths, which reading media files allows.
 */
object DeviceMusic {
    /** READ_MEDIA_AUDIO from Android 13, READ_EXTERNAL_STORAGE before. */
    val AUDIO_PERMISSION = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        Manifest.permission.READ_MEDIA_AUDIO
    } else {
        Manifest.permission.READ_EXTERNAL_STORAGE
    }

    /** Folders with music in them, by name. Slow: call it off the main thread. */
    fun folders(context: Context): List<MusicFolder> {
        val volumes = volumes(context)
        val byFolder = LinkedHashMap<String, MutableList<String>>()
        val names = HashMap<String, Pair<String, String>>()
        for (path in musicFiles(context)) {
            val root = volumes.keys.filter { path.startsWith("$it/") }.maxByOrNull { it.length } ?: File(path).parent ?: continue
            // The first folder under the volume; files right at the top go together as the volume.
            val first = path.removePrefix("$root/").substringBefore('/', "")
            val folder = if (first.isEmpty()) root else "$root/$first"
            byFolder.getOrPut(folder) { ArrayList() }.add(path)
            names.getOrPut(folder) {
                val volume = volumes[root] ?: File(root).name
                (if (folder == root) volume else first) to volume
            }
        }
        return byFolder.map { (folder, files) ->
            val (name, volume) = names.getValue(folder)
            MusicFolder(folder, name, volume, files.sorted())
        }.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })
    }

    // MediaStore's music: not ringtones, notification sounds or recordings.
    private fun musicFiles(context: Context): List<String> {
        val uri = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            MediaStore.Audio.Media.getContentUri(MediaStore.VOLUME_EXTERNAL)
        } else {
            MediaStore.Audio.Media.EXTERNAL_CONTENT_URI
        }
        @Suppress("DEPRECATION") // DATA is still the file's path, which is what the core needs.
        val data = MediaStore.Audio.Media.DATA
        val files = ArrayList<String>()
        try {
            context.contentResolver.query(uri, arrayOf(data), "${MediaStore.Audio.Media.IS_MUSIC} != 0", null, null)?.use { cursor ->
                val column = cursor.getColumnIndexOrThrow(data)
                while (cursor.moveToNext()) cursor.getString(column)?.let(files::add)
            }
        } catch (e: SecurityException) {
            Log.w(TAG, "No permission to read music", e)
        }
        return files
    }

    // Each storage volume's folder, with its name: "Internal storage" or an SD card's label.
    private fun volumes(context: Context): Map<String, String> {
        val storage = context.getSystemService(StorageManager::class.java)
        val volumes = HashMap<String, String>()
        val internal = context.getString(R.string.internal_storage)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            for (volume in storage.storageVolumes) {
                val dir = volume.directory ?: continue
                volumes[dir.path] = if (volume.isPrimary) internal else volume.getDescription(context)
            }
        } else {
            @Suppress("DEPRECATION")
            volumes[Environment.getExternalStorageDirectory().path] = internal
            // Removable volumes are mounted as /storage/<id>.
            File("/storage").listFiles()?.filter { it.name != "emulated" && it.name != "self" }?.forEach { dir ->
                volumes[dir.path] = storage.getStorageVolume(dir)?.getDescription(context) ?: dir.name
            }
        }
        return volumes
    }

    private const val TAG = "musicapp"
}
