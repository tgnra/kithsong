package ng.kithso.android.playback

import android.content.ClipDescription
import android.content.ContentProvider
import android.content.ContentValues
import android.content.Context
import android.database.Cursor
import android.database.MatrixCursor
import android.graphics.Bitmap
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import android.util.Log
import ng.kithso.android.KithsongApp
import ng.kithso.android.data.ArtRef
import ng.kithso.android.ui.art.loadArtBitmap
import kotlinx.coroutines.runBlocking
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.security.MessageDigest

/**
 * Serves album art as JPEGs to other apps, for Android Auto's browse lists: Auto loads an
 * item's art itself, from its artwork URI, and can't read files in app storage.
 *
 * A URI says all there is to know about the art, so nothing is looked up: the cover file as a
 * path under one of the folders covers live in (the library, or the playlists' pictures), its
 * version, so a changed cover gets a new URI, and the uid and label the placeholder is drawn
 * from when there's no file, or it won't decode:
 * `content://<package>.art/library/<path in the library>?uid=…&label=…&v=<version>`, or
 * `content://<package>.art/none?uid=…&label=…` for a placeholder.
 *
 * Files are never served as they are: the art is decoded through the app's image loader, as
 * the app shows it, and encoded again, so a URI naming some other file in those folders gets
 * only a placeholder. Encoded art is kept in the cache, so asking again is cheap.
 */
class ArtProvider : ContentProvider() {
    override fun onCreate() = true

    override fun getType(uri: Uri) = MIME_TYPE

    override fun getStreamTypes(uri: Uri, mimeTypeFilter: String): Array<String>? =
        arrayOf(MIME_TYPE).takeIf { ClipDescription.compareMimeTypes(MIME_TYPE, mimeTypeFilter) }

    // Calls come in on binder threads, so blocking on the image loader here is fine.
    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        if (mode != "r") throw SecurityException("Art is read-only")
        val context = context ?: throw FileNotFoundException(uri.toString())
        val art = artFor(context, uri) ?: throw FileNotFoundException(uri.toString())
        return ParcelFileDescriptor.open(encoded(context, art), ParcelFileDescriptor.MODE_READ_ONLY)
    }

    // Enough for apps that ask what they're opening before they open it.
    override fun query(uri: Uri, projection: Array<String>?, selection: String?, selectionArgs: Array<String>?, sortOrder: String?): Cursor? {
        val context = context ?: return null
        val art = artFor(context, uri) ?: return null
        val columns = projection ?: arrayOf(OpenableColumns.DISPLAY_NAME)
        return MatrixCursor(columns, 1).apply {
            addRow(columns.map { if (it == OpenableColumns.DISPLAY_NAME) "${art.label.ifEmpty { art.uid }}.jpg" else null })
        }
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? = throw UnsupportedOperationException()

    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<String>?): Int =
        throw UnsupportedOperationException()

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<String>?): Int = throw UnsupportedOperationException()

    // [art] as a JPEG in the cache, encoded now if it isn't there yet. The name is a hash of
    // all that makes the image, so a changed cover or label is a new file.
    private fun encoded(context: Context, art: ArtRef): File {
        val dir = File(context.cacheDir, CACHE_DIR)
        val key = "${art.uid}\u0000${art.label}\u0000${art.file}\u0000${art.version}\u0000$ART_PX"
        val name = MessageDigest.getInstance("SHA-1").digest(key.toByteArray()).joinToString("") { "%02x".format(it) }
        val file = File(dir, "$name.jpg")
        if (file.isFile) {
            file.setLastModified(System.currentTimeMillis())
            return file
        }
        val bitmap = runBlocking { loadArtBitmap(context, art, ART_PX) } ?: throw FileNotFoundException("No art for ${art.uid}")
        dir.mkdirs()
        // Written aside and moved into place, so another thread asking for the same art never
        // opens it half written.
        val temp = File.createTempFile(name, ".tmp", dir)
        try {
            temp.outputStream().use { if (!bitmap.compress(Bitmap.CompressFormat.JPEG, 90, it)) throw IOException("Couldn't encode") }
            if (!temp.renameTo(file)) throw IOException("Couldn't move $temp to $file")
        } catch (e: IOException) {
            temp.delete()
            Log.e(TAG, "Couldn't keep art for ${art.uid}", e)
            throw FileNotFoundException(e.message)
        }
        prune(dir)
        return file
    }

    // Keeps the cache to the art used most recently.
    private fun prune(dir: File) {
        val files = dir.listFiles()?.takeIf { it.size > MAX_CACHED } ?: return
        files.sortedByDescending { it.lastModified() }.drop(MAX_CACHED * 3 / 4).forEach { it.delete() }
    }

    companion object {
        private const val TAG = "kithsong"
        private const val MIME_TYPE = "image/jpeg"
        private const val CACHE_DIR = "auto-art"
        private const val MAX_CACHED = 400
        // Big enough for a car's browse lists and its now-playing screen.
        private const val ART_PX = 512
        private const val PLACEHOLDER = "none"
        private const val UID = "uid"
        private const val LABEL = "label"
        private const val VERSION = "v"

        /** A content:// URI that serves [art] (cover file or placeholder) as an image, readable by Android Auto. */
        fun uriFor(context: Context, art: ArtRef): Uri {
            val builder = Uri.Builder().scheme("content").authority(authority(context))
            val file = art.file?.let { File(it) }
            val root = file?.let { roots(context).entries.firstOrNull { (_, dir) -> isIn(file, dir) } }
            if (file != null && root != null) {
                builder.appendPath(root.key)
                file.canonicalPath.removePrefix(root.value.canonicalPath + File.separator).split(File.separator).forEach(builder::appendPath)
                builder.appendQueryParameter(VERSION, art.version.toString())
            } else {
                // Not in a folder art is served from: the placeholder, as the app shows for art it can't decode.
                builder.appendPath(PLACEHOLDER)
            }
            return builder.appendQueryParameter(UID, art.uid).appendQueryParameter(LABEL, art.label).build()
        }

        /** The art [uri] serves, if it's one of [uriFor]'s, with its file only if that's where art may be. */
        fun artFor(context: Context, uri: Uri): ArtRef? {
            if (uri.scheme != "content" || uri.authority != authority(context)) return null
            val uid = uri.getQueryParameter(UID) ?: return null
            val label = uri.getQueryParameter(LABEL).orEmpty()
            val segments = uri.pathSegments
            val root = segments.firstOrNull() ?: return null
            if (root == PLACEHOLDER) return ArtRef(uid, label)
            val dir = roots(context)[root] ?: return null
            val parts = segments.drop(1)
            if (parts.isEmpty() || parts.any { it.isEmpty() || it == "." || it == ".." || File.separator in it }) return null
            val file = File(dir, parts.joinToString(File.separator))
            if (!isIn(file, dir)) return null
            return ArtRef(uid, label, file.path, uri.getQueryParameter(VERSION)?.toLongOrNull() ?: 0)
        }

        private fun authority(context: Context) = "${context.packageName}.art"

        // The folders covers live in: album folders in the library, and the pictures the core
        // makes for playlists and album groups.
        private fun roots(context: Context): Map<String, File> {
            val app = context.applicationContext as KithsongApp
            return mapOf("library" to app.libraryDir, "playlists" to app.playlistArtDir)
        }

        // Whether [file] is inside [dir], once symlinks and ".." are resolved.
        private fun isIn(file: File, dir: File): Boolean = try {
            file.canonicalPath.startsWith(dir.canonicalPath + File.separator)
        } catch (e: IOException) {
            false
        }
    }
}
