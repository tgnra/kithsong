package ng.kithso.android.data

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import ng.kithso.core.CoreLibrary
import ng.kithso.core.NativeCore
import ng.kithso.core.TrackInfo
import ng.kithso.core.artistSingles
import java.io.BufferedOutputStream
import java.io.File
import java.io.IOException
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive

/**
 * Exporting the library's music as zip files, before a new library replaces it ([ng.kithso.android.NewLibrary]).
 * Each zip goes back in through Import, which extracts it and imports the folder (see "Zips"
 * in kithcore/tagger.h), so each holds whole releases, laid out as the library folder has them
 * (Artist/Album (Year)/01 Title.flac, with the album's cover beside its tracks), and no more
 * than the importer takes from one zip ([MAX_ZIP_BYTES]). Only files: playlists, plays and the
 * rest of library.db stay behind. Tag edits are in the files, so they go too.
 */

/** The most the importer extracts from one zip, unzipped (MC_UNZIP_MAX_BYTES). */
const val MAX_ZIP_BYTES = 8L * 1024 * 1024 * 1024

/** A file to export: [file] on this phone, in the zip as [entry]. */
data class ExportFile(val file: File, val entry: String, val bytes: Long)

/** A release's files (an album's, or a single's), which go in the same zip. */
data class ExportRelease(val files: List<ExportFile>) {
    val bytes: Long get() = files.sumOf { it.bytes }
}

/**
 * [releases] shared out into zips of at most [maxBytes] each, in order, never splitting one: a
 * release goes in the zip being filled if it fits, else it starts the next. One bigger than
 * [maxBytes] has a zip of its own, which the importer will refuse whole (it can be unzipped and
 * imported as a folder), as halves of an album would be tagged as two.
 */
fun <T> splitReleases(releases: List<T>, maxBytes: Long, bytes: (T) -> Long): List<List<T>> {
    val zips = ArrayList<List<T>>()
    var zip = ArrayList<T>()
    var size = 0L
    for (release in releases) {
        val n = bytes(release)
        if (zip.isNotEmpty() && size + n > maxBytes) {
            zips += zip
            zip = ArrayList()
            size = 0
        }
        zip += release
        size += n
    }
    if (zip.isNotEmpty()) zips += zip
    return zips
}

/**
 * The library's files on this phone, a release at a time, in the album list's order: every
 * album with a file here, then the singles and tracks on no album, which the album list leaves
 * out (each artist's, from mc_library_list_artist_singles). Each track goes once, as its file
 * (the original, if a copy is here too), with its album's cover. [dir] is the library folder,
 * which entries are named from. A single or loose track credited to no artist at all isn't
 * listed anywhere, so it's left out.
 */
fun exportReleases(lib: CoreLibrary, dir: File): List<ExportRelease> {
    val seen = HashSet<String>()
    val releases = ArrayList<ExportRelease>()
    fun add(tracks: List<TrackInfo>, coverDirs: List<String?>) {
        val here = tracks.filter { it.path != null && seen.add(it.uid) }
        if (here.isEmpty()) return
        val covers = (coverDirs + here.map { it.coverDir }).filterNotNull().distinct().mapNotNull(NativeCore::coverFile)
        val files = (here.map { File(it.path!!) } + covers.map(::File)).distinct().map { file ->
            ExportFile(file, entryName(file, dir), file.length())
        }
        releases += ExportRelease(files)
    }
    for (album in lib.albums()) {
        val folder = album.folder ?: continue
        add(lib.albumTracks(album.uid), listOf(folder))
    }
    for (artist in lib.artistsHere()) {
        for ((_, tracks) in lib.artistSingles(artist.uid).groupBy { it.albumUid ?: it.uid }) add(tracks, emptyList())
    }
    return releases
}

// [file]'s place in the library folder, with '/' between folders; just its name if it's
// somewhere else.
private fun entryName(file: File, dir: File): String =
    file.relativeToOrNull(dir)?.invariantSeparatorsPath?.takeIf { !it.startsWith("..") } ?: file.name

/**
 * How far an export is: writing zip [zip] (from 1) of [zips], [done] of [total] bytes read
 * (each file is read twice, see [writeZips]).
 */
data class ExportProgress(val zip: Int, val zips: Int, val done: Long, val total: Long) {
    val fraction: Float get() = if (total > 0) (done.toDouble() / total).toFloat().coerceIn(0f, 1f) else 0f

    /** The size of what's exported, and about how much of it is in the zips so far. */
    val bytes: Long get() = total / 2
    val bytesDone: Long get() = done / 2
}

/** A file of the library couldn't be read: [path], relative to the library folder. */
class ExportReadException(val path: String, cause: Throwable) : IOException("Couldn't read $path", cause)

/**
 * Writes [zips] into the folder [tree] (from OpenDocumentTree) as "[name].zip", or "[name] 1.zip",
 * "[name] 2.zip" and so on with several, straight from the library's files to the documents, so
 * nothing is copied in the app's storage first. Entries are stored, not compressed: audio
 * doesn't shrink. Each file is read twice, for its CRC and then its bytes, as a stored entry
 * needs its CRC before them. Cancelling the caller stops it. If it doesn't finish, every zip
 * it made is deleted again, as a part of the library might be taken for all of it. Returns how
 * many it wrote. Throws [ExportReadException] for a file it couldn't read, and IOException
 * for one it couldn't write.
 */
suspend fun writeZips(
    resolver: ContentResolver,
    tree: Uri,
    name: String,
    zips: List<List<ExportRelease>>,
    onProgress: (ExportProgress) -> Unit,
): Int {
    val folder = DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree))
    val total = zips.sumOf { zip -> zip.sumOf { it.bytes } } * 2 // each byte read twice
    var done = 0L
    val made = ArrayList<Uri>()
    try {
        for ((i, zip) in zips.withIndex()) {
            val title = if (zips.size == 1) "$name.zip" else "$name ${i + 1}.zip"
            val uri = DocumentsContract.createDocument(resolver, folder, ZIP_TYPE, title)
                ?: throw IOException("Couldn't create $title")
            made += uri
            val out = resolver.openOutputStream(uri, "w") ?: throw IOException("Couldn't open $title")
            ZipOutputStream(BufferedOutputStream(out, BUFFER_BYTES)).use { stream ->
                val entries = HashSet<String>() // a cover two albums share goes once
                for (file in zip.flatMap { it.files }) {
                    if (!entries.add(file.entry)) continue
                    done = writeEntry(stream, file, done) { onProgress(ExportProgress(i + 1, zips.size, it, total)) }
                }
            }
        }
        return made.size
    } catch (e: Throwable) {
        for (uri in made) {
            try {
                DocumentsContract.deleteDocument(resolver, uri)
            } catch (d: Exception) {
                Log.w(TAG, "Couldn't delete $uri", d)
            }
        }
        throw e
    }
}

// One stored entry; returns [done] plus the bytes read, which it passes to [progress] as it goes.
private suspend fun writeEntry(stream: ZipOutputStream, file: ExportFile, done: Long, progress: (Long) -> Unit): Long {
    val buffer = ByteArray(BUFFER_BYTES)
    var read = done
    var lastShown = read
    // Only reading fails with ExportReadException; writing's own IOException goes through.
    suspend fun pass(each: (Int) -> Unit): Long {
        var size = 0L
        val input = try {
            file.file.inputStream()
        } catch (e: IOException) {
            throw ExportReadException(file.entry, e)
        }
        input.use {
            while (true) {
                val n = try {
                    input.read(buffer)
                } catch (e: IOException) {
                    throw ExportReadException(file.entry, e)
                }
                if (n < 0) break
                each(n)
                size += n
                read += n
                if (read - lastShown >= PROGRESS_BYTES) {
                    currentCoroutineContext().ensureActive()
                    lastShown = read
                    progress(read)
                }
            }
        }
        return size
    }
    val crc = CRC32()
    val size = pass { n -> crc.update(buffer, 0, n) }
    val entry = ZipEntry(file.entry).apply {
        method = ZipEntry.STORED
        this.size = size
        compressedSize = size
        this.crc = crc.value
        time = file.file.lastModified()
    }
    stream.putNextEntry(entry)
    // The zip checks the size and CRC against the entry's, so a file that changed between
    // the passes fails the export rather than going in damaged.
    pass { n -> stream.write(buffer, 0, n) }
    stream.closeEntry()
    progress(read)
    return read
}

private const val TAG = "kithsong"
private const val ZIP_TYPE = "application/zip"
private const val BUFFER_BYTES = 256 * 1024
private const val PROGRESS_BYTES = 4L * 1024 * 1024
