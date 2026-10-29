package ng.kithso.android.importer

import android.content.ContentResolver
import android.content.Context
import android.database.Cursor
import android.net.Uri
import android.os.StatFs
import android.provider.DocumentsContract
import android.provider.DocumentsContract.Document
import android.provider.OpenableColumns
import android.text.format.Formatter
import android.util.Log
import android.webkit.MimeTypeMap
import ng.kithso.android.R
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import java.io.File
import java.io.FileOutputStream
import java.io.IOException

/**
 * What [Importer] copies out of the Storage Access Framework into the app's cache: plans the
 * batches of a picked folder or of picked documents, then copies a batch's files under a
 * folder of its own. Blocking; run it on Dispatchers.IO. What goes wrong is collected for
 * [takeProblems], in the user's language.
 */
internal class StagedFiles(private val context: Context, private val root: File) {
    /** One file to copy: [path] is where it goes, relative to the batch's folder, '/' between parts. */
    class Item(val uri: Uri, val path: String, val size: Long)

    /** The files of one session: an album folder with its disc folders, the documents' audio, or one zip. */
    class Batch(val name: String, val files: List<Item>, val isZip: Boolean) {
        val size: Long get() = files.sumOf { it.size.coerceAtLeast(0) }
    }

    private val resolver: ContentResolver = context.contentResolver
    private val problems = mutableListOf<String>()

    @Synchronized
    fun takeProblems(): List<String> = problems.toList().also { problems.clear() }

    @Synchronized
    private fun problem(text: String) {
        problems += text
    }

    // --- Planning ---

    private class Entry(val uri: Uri, val rel: String, val size: Long)

    /**
     * The batches of the tree [tree]: its audio files grouped by album folder (a disc folder
     * such as "CD1" goes with its parent), with the covers and other pictures in those folders,
     * and each zip on its own. Paths keep the picked folder's name and everything under it.
     */
    fun planFolder(tree: Uri): List<Batch> {
        val rootId = DocumentsContract.getTreeDocumentId(tree)
        val rootName = query(DocumentsContract.buildDocumentUriUsingTree(tree, rootId), arrayOf(Document.COLUMN_DISPLAY_NAME)) {
            if (it.moveToFirst()) it.getString(0) else null
        }?.let(::safeName) ?: "Import"

        val found = mutableListOf<Entry>()
        // Breadth first, without recursion: a deep tree can't overflow the stack.
        val pending = ArrayDeque(listOf(rootId to rootName))
        while (pending.isNotEmpty()) {
            val (id, rel) = pending.removeFirst()
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, id)
            query(children, CHILD_COLUMNS) { c ->
                while (c.moveToNext()) {
                    val childId = c.getString(0) ?: continue
                    val name = c.getString(1) ?: continue
                    if (name.startsWith(".")) continue // hidden, as the core skips them
                    val mime = c.getString(2)
                    val path = "$rel/${safeName(name)}"
                    if (mime == Document.MIME_TYPE_DIR) {
                        pending += childId to path
                    } else if (isAudio(name) || isPicture(name) || isZip(name, mime)) {
                        val size = if (c.isNull(3)) -1L else c.getLong(3)
                        found += Entry(DocumentsContract.buildDocumentUriUsingTree(tree, childId), path, size)
                    }
                }
            }
        }

        val zips = found.filter { isZip(it.rel, null) }
        val byFolder = found.filterNot { isZip(it.rel, null) }.groupBy { albumFolder(it.rel) }
        val batches = byFolder.entries
            .filter { (_, files) -> files.any { isAudio(it.rel) } }
            .sortedBy { it.key }
            .map { (folder, files) ->
                Batch(folder.substringAfterLast('/'), files.map { Item(it.uri, it.rel, it.size) }, isZip = false)
            }
        return batches + zips.sortedBy { it.rel }.map { zip ->
            Batch(zip.rel.substringAfterLast('/'), listOf(Item(zip.uri, zip.rel.substringAfterLast('/'), zip.size)), isZip = true)
        }
    }

    /**
     * The batches of documents picked or shared: their audio files together, flat, so files
     * of one album group together, and each zip on its own. Anything else is left out.
     */
    fun planDocuments(uris: List<Uri>): List<Batch> {
        val audio = mutableListOf<Item>()
        val zips = mutableListOf<Batch>()
        val used = mutableSetOf<String>()
        for (uri in uris.distinct()) {
            val (shown, size) = query(uri, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)) {
                if (it.moveToFirst()) {
                    val n = it.getColumnIndex(OpenableColumns.DISPLAY_NAME).takeIf { i -> i >= 0 }?.let(it::getString)
                    val s = it.getColumnIndex(OpenableColumns.SIZE).takeIf { i -> i >= 0 && !it.isNull(i) }?.let(it::getLong)
                    n to (s ?: -1L)
                } else {
                    null to -1L
                }
            } ?: (null to -1L)
            val mime = try {
                resolver.getType(uri)
            } catch (e: Exception) {
                null
            }
            var name = safeName(shown ?: uri.lastPathSegment?.substringAfterLast('/') ?: "file")
            // The core goes by the extension; a provider may leave it off the name.
            val ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(mime)
            if (isZip(name, mime) && !name.endsWith(".zip", ignoreCase = true)) name += ".zip"
            else if (!isAudio(name) && mime?.startsWith("audio/") == true && ext != null) name += ".$ext"
            name = unique(name, used)
            when {
                isZip(name, mime) -> zips += Batch(name, listOf(Item(uri, name, size)), isZip = true)
                isAudio(name) -> audio += Item(uri, name, size)
                else -> problem(context.getString(R.string.import_not_music, name))
            }
        }
        val name = context.getString(R.string.import_files_folder)
        return listOfNotNull(audio.takeIf { it.isNotEmpty() }?.let { files ->
            // In a folder of their own inside the batch, as a folder import has.
            Batch(name, files.map { Item(it.uri, "$name/${it.path}", it.size) }, isZip = false)
        }) + zips
    }

    // --- Copying ---

    /**
     * Copies [batch] into [dir], calling [onFile] after each file. Returns what to give the
     * core: [dir], or the zip's copy. Null if nothing could be copied (a problem is noted).
     */
    suspend fun copy(batch: Batch, dir: File, onFile: suspend () -> Unit): File? {
        // The copy, and then the library's copy of it, both in the app's storage.
        val free = StatFs(root.apply { mkdirs() }.path).availableBytes
        if (batch.size * 2 + SPACE_MARGIN > free) {
            problem(context.getString(R.string.import_no_space, batch.name, Formatter.formatShortFileSize(context, batch.size)))
            return null
        }
        var copied = 0
        var first: File? = null
        for (file in batch.files) {
            currentCoroutineContext().ensureActive()
            val dest = File(dir, file.path)
            try {
                dest.parentFile?.mkdirs()
                val input = resolver.openInputStream(file.uri) ?: throw IOException("no stream")
                input.use { from ->
                    FileOutputStream(dest).use { to ->
                        val buffer = ByteArray(BUFFER_SIZE)
                        while (true) {
                            val n = from.read(buffer)
                            if (n < 0) break
                            to.write(buffer, 0, n)
                            currentCoroutineContext().ensureActive()
                        }
                    }
                }
                copied++
                if (first == null) first = dest
            } catch (e: IOException) {
                Log.w(TAG, "Couldn't copy ${file.uri}", e)
                dest.delete()
                problem(context.getString(R.string.import_copy_failed, file.path.substringAfterLast('/')))
            } catch (e: SecurityException) {
                Log.w(TAG, "No access to ${file.uri}", e)
                dest.delete()
                problem(context.getString(R.string.import_copy_failed, file.path.substringAfterLast('/')))
            } catch (e: IllegalArgumentException) {
                Log.w(TAG, "Gone: ${file.uri}", e)
                problem(context.getString(R.string.import_copy_failed, file.path.substringAfterLast('/')))
            }
            onFile()
        }
        if (copied == 0) {
            dir.deleteRecursively()
            return null
        }
        return if (batch.isZip) first else dir
    }

    // --- Helpers ---

    private fun <T> query(uri: Uri, columns: Array<String>, read: (Cursor) -> T): T? = try {
        resolver.query(uri, columns, null, null, null)?.use(read)
    } catch (e: Exception) {
        // SecurityException, or a provider that's gone: what was found stays.
        Log.w(TAG, "Couldn't list $uri", e)
        problem(context.getString(R.string.import_read_failed))
        null
    }

    private companion object {
        const val TAG = "kithsong"
        const val BUFFER_SIZE = 256 * 1024
        const val SPACE_MARGIN = 64L * 1024 * 1024
        val CHILD_COLUMNS = arrayOf(
            Document.COLUMN_DOCUMENT_ID,
            Document.COLUMN_DISPLAY_NAME,
            Document.COLUMN_MIME_TYPE,
            Document.COLUMN_SIZE,
        )

        // The core's audio extensions (tagger.c's audio_format).
        val AUDIO = setOf("flac", "mp3", "opus", "wav", "aif", "aiff")
        // Covers the core picks up (coverart.c), and other pictures of the album.
        val PICTURES = setOf("jpg", "jpeg", "png")
        val ZIP_TYPES = setOf("application/zip", "application/x-zip-compressed", "application/x-zip")
        val DISC = Regex("(?i)(cd|disc|disk)[ _.-]*\\d+")

        fun extension(name: String) = name.substringAfterLast('.', "").lowercase()
        fun isAudio(name: String) = extension(name) in AUDIO
        fun isPicture(name: String) = extension(name) in PICTURES
        fun isZip(name: String, mime: String?) = extension(name) == "zip" || mime in ZIP_TYPES

        // The album folder of a file: its folder, or the one above for a disc folder.
        fun albumFolder(rel: String): String {
            val folder = rel.substringBeforeLast('/', "")
            val name = folder.substringAfterLast('/')
            return if (DISC.matches(name) && '/' in folder) folder.substringBeforeLast('/') else folder
        }

        // A name the file system takes: no separators or NULs, not "." or "..".
        fun safeName(name: String): String {
            val clean = name.replace('/', '_').replace('\u0000', '_').trim()
            return if (clean.isEmpty() || clean == "." || clean == "..") "_" else clean.take(200)
        }

        fun unique(name: String, used: MutableSet<String>): String {
            var candidate = name
            var n = 2
            while (!used.add(candidate.lowercase())) {
                val dot = name.lastIndexOf('.')
                candidate = if (dot > 0) "${name.substring(0, dot)} ($n)${name.substring(dot)}" else "$name ($n)"
                n++
            }
            return candidate
        }
    }
}
