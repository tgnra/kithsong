package ng.kithso.android.data

import android.util.Log
import androidx.compose.runtime.Immutable
import ng.kithso.core.AlbumFlags
import ng.kithso.core.AlbumTags
import ng.kithso.core.ArtistTags
import ng.kithso.core.CoreEdit
import ng.kithso.core.CoreLibrary
import ng.kithso.core.MusicCoreException
import ng.kithso.core.ReleaseInfo
import ng.kithso.core.TrackTags
import ng.kithso.core.albumArt
import ng.kithso.core.albumFlags
import ng.kithso.core.albumTags
import ng.kithso.core.artistTags
import ng.kithso.core.releaseTags
import ng.kithso.core.searchReleases
import ng.kithso.core.setAlbumArt
import ng.kithso.core.setAlbumFlags
import ng.kithso.core.setAlbumTags
import ng.kithso.core.setArtistTags
import ng.kithso.core.setTrackMix
import ng.kithso.core.setTrackTags
import ng.kithso.core.trackMix
import ng.kithso.core.trackTags
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.io.InputStream

/** What the tag editor opens on: an album, artist or track. */
@Immutable
data class TagTarget(val kind: Kind, val uid: String) {
    enum class Kind { Album, Artist, Track }
}

/** What the tag editor edits, as the core has it: an album's flags and cover go with its tags, and a single's mix with its. */
@Immutable
sealed interface TagValues {
    /**
     * [art] is the cover it has, null for none, and [canSetArt] whether it can have another: not
     * with no file here for one to go beside. [newArt] is one chosen for Save to keep.
     */
    data class Album(
        val tags: AlbumTags,
        val flags: AlbumFlags,
        val art: ArtRef? = null,
        val canSetArt: Boolean = true,
        val newArt: ArtRef? = null,
    ) : TagValues
    data class Artist(val tags: ArtistTags) : TagValues

    /** [canSetMix]: it's a single, whose flag is its own. On a longer album [mix] means nothing here. */
    data class Track(val tags: TrackTags, val canSetMix: Boolean = false, val mix: Boolean = false) : TagValues
}

/** A [value], or the core's [Failed.status], for [ng.kithso.core.CoreEdit] to tell apart and put in words. */
sealed interface TagResult<out T> {
    data class Ok<T>(val value: T) : TagResult<T>
    data class Failed(val status: Int) : TagResult<Nothing>
}

/**
 * How a save went: [saved] is what the library has now, and [status] the failure, 0 if there was
 * none. [artTried]: it had got as far as the cover, so a file in use isn't one being played.
 */
@Immutable
data class TagSave(val saved: TagValues, val status: Int, val artTried: Boolean = false)

/** All of [stream], or null if that's more than [limit] bytes. */
internal fun readAtMost(stream: InputStream, limit: Int): ByteArray? {
    val out = ByteArrayOutputStream()
    val buffer = ByteArray(64 * 1024)
    while (true) {
        val n = stream.read(buffer)
        if (n < 0) return out.toByteArray()
        if (out.size() + n > limit) return null
        out.write(buffer, 0, n)
    }
}

/**
 * Editing an album's, artist's or track's tags, an album's flags and cover and a single's mix, and
 * matching an album to another MusicBrainz release, for a [LibrarySource] that can ([CoreTagEditor]; the made-up
 * library can't, and the UI then offers no Edit). What an edit does, to rows and files, is
 * all the core's (musiccore/edit.h).
 */
interface TagEditor {
    suspend fun read(target: TagTarget): TagResult<TagValues>

    /**
     * Saves what changed from [was] to [now]: the tags, then an album's flags or a single's mix,
     * then an album's new cover, last as the tags can move its folder. A failure can leave some
     * saved and not the rest, which [TagSave.saved] says, for a second try to leave them out.
     * It's seen through, and the library read again, even if the caller goes.
     */
    suspend fun save(target: TagTarget, was: TagValues, now: TagValues): TagSave

    /**
     * What [open] gives, if it makes a cover, set aside for album [albumUid] to keep on Save and
     * to show as [TagValues.Album.newArt] until then; null if it doesn't, or can't be read. It
     * replaces any set aside before. [open] may throw as a content resolver does.
     */
    suspend fun takeCover(albumUid: String, open: () -> InputStream?): ArtRef?

    /** Forgets a cover [takeCover] set aside, the form having finished with it. */
    fun dropCover(art: ArtRef)

    /** The releases for [artist] and [title], best fit for album [albumUid]'s tracks first. */
    suspend fun searchReleases(albumUid: String, artist: String, title: String): TagResult<List<ReleaseInfo>>

    /** What album [albumUid]'s tags would be on release [mbAlbumId]; nothing changes until a save. */
    suspend fun releaseTags(albumUid: String, mbAlbumId: String): TagResult<AlbumTags>

    /** [queue] as the library has its tracks now: an edit can move their files and retitle them. */
    suspend fun freshTracks(queue: List<Track>): List<Track>
}

/**
 * [TagEditor] on the core. A save can take as long as copying the files it touches, and a search
 * about 6 s, so each has a thread and library connection of its own, and the library's own
 * thread stays free for the app; a search left running when its page closes (there's no
 * stopping a network call) holds up nothing but the next search. [changed] reads the library
 * again and passes the change on to sync; [fresh] is [freshTracks].
 *
 * A cover chosen and not saved yet is a file in [coverDir], not bytes in the form: it can be 16 MB,
 * which a rotation couldn't keep, and a picked URI's permission needn't outlast the activity. In
 * the cache, so one left behind by a process that died can do no harm; the next replaces it.
 */
class CoreTagEditor(
    private val dir: File,
    private val coverDir: File,
    private val scope: CoroutineScope,
    private val changed: suspend () -> Unit,
    private val fresh: suspend (List<Track>) -> List<Track>,
) : TagEditor {
    private val saving = Dispatchers.IO.limitedParallelism(1)
    private var saveLib: CoreLibrary? = null // only touched on [saving]
    private val searching = Dispatchers.IO.limitedParallelism(1)
    private var searchLib: CoreLibrary? = null // only touched on [searching]

    override suspend fun read(target: TagTarget): TagResult<TagValues> = withContext(saving) {
        attempt("read $target") {
            val lib = saveLib ?: CoreLibrary(dir.path).also { saveLib = it }
            when (target.kind) {
                TagTarget.Kind.Album -> readAlbum(lib, target.uid)
                TagTarget.Kind.Artist -> TagValues.Artist(lib.artistTags(target.uid))
                TagTarget.Kind.Track -> lib.trackMix(target.uid).let { TagValues.Track(lib.trackTags(target.uid), it.canSet, it.mix) }
            }
        }
    }

    private fun readAlbum(lib: CoreLibrary, uid: String): TagValues.Album {
        val tags = lib.albumTags(uid)
        val flags = lib.albumFlags(uid)
        return try {
            val art = lib.albumArt(uid)?.let { ArtRef(uid, tags.title, it, File(it).lastModified()) }
            TagValues.Album(tags, flags, art)
        } catch (e: MusicCoreException) {
            // The album's there, so it's that it has no file here: no folder for a cover.
            if (e.status != CoreEdit.ERR_NOT_FOUND) throw e
            TagValues.Album(tags, flags, canSetArt = false)
        }
    }

    // In [scope], so it's seen through and the library read again even if the screen goes away.
    override suspend fun save(target: TagTarget, was: TagValues, now: TagValues): TagSave = scope.async(saving) {
        var saved = was
        var status = 0
        var artTried = false
        try {
            val lib = saveLib ?: CoreLibrary(dir.path).also { saveLib = it }
            when {
                was is TagValues.Album && now is TagValues.Album -> {
                    if (now.tags != was.tags) {
                        lib.setAlbumTags(target.uid, now.tags)
                        saved = was.copy(tags = now.tags)
                    }
                    if (now.flags != was.flags) {
                        lib.setAlbumFlags(target.uid, now.flags)
                        saved = (saved as TagValues.Album).copy(flags = now.flags)
                    }
                    val art = now.newArt?.file
                    if (art != null && now.newArt != was.newArt) {
                        artTried = true
                        lib.setAlbumArt(target.uid, File(art).readBytes())
                        saved = now
                    }
                }
                was is TagValues.Artist && now is TagValues.Artist -> {
                    lib.setArtistTags(target.uid, now.tags)
                    saved = now
                }
                was is TagValues.Track && now is TagValues.Track -> {
                    if (now.tags != was.tags) {
                        lib.setTrackTags(target.uid, now.tags)
                        saved = was.copy(tags = now.tags)
                    }
                    if (now.mix != was.mix) {
                        lib.setTrackMix(target.uid, now.mix)
                        saved = now
                    }
                }
            }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't save $target", e)
            status = e.status
        } catch (e: IOException) {
            // The cover set aside has gone from the cache.
            Log.e(TAG, "Couldn't read the cover for $target", e)
            status = CoreEdit.ERR_IO
        }
        if (saved != was) changed()
        TagSave(saved, status, artTried)
    }.await()

    override suspend fun takeCover(albumUid: String, open: () -> InputStream?): ArtRef? = withContext(Dispatchers.IO) {
        try {
            val image = open()?.use { readAtMost(it, CoreEdit.ALBUM_ART_MAX_BYTES) } ?: return@withContext null
            CoreEdit.checkAlbumArt(image)
            coverDir.mkdirs()
            val file = File.createTempFile("cover", null, coverDir)
            file.writeBytes(image)
            coverDir.listFiles()?.filter { it != file }?.forEach(File::delete)
            ArtRef(albumUid, "", file.path, file.lastModified())
        } catch (e: MusicCoreException) {
            Log.i(TAG, "That can't be a cover: ${e.status}")
            null
        } catch (e: IOException) {
            Log.e(TAG, "Couldn't read a cover", e)
            null
        } catch (e: SecurityException) {
            Log.e(TAG, "Couldn't read a cover", e)
            null
        }
    }

    override fun dropCover(art: ArtRef) {
        art.file?.let { File(it).delete() }
    }

    override suspend fun searchReleases(albumUid: String, artist: String, title: String): TagResult<List<ReleaseInfo>> =
        withContext(searching) {
            attempt("search for $artist, $title") { searchConnection().searchReleases(albumUid, artist, title) }
        }

    override suspend fun releaseTags(albumUid: String, mbAlbumId: String): TagResult<AlbumTags> = withContext(searching) {
        attempt("get release $mbAlbumId") { searchConnection().releaseTags(albumUid, mbAlbumId) }
    }

    override suspend fun freshTracks(queue: List<Track>): List<Track> = fresh(queue)

    private fun searchConnection() = searchLib ?: CoreLibrary(dir.path).also { searchLib = it }

    private inline fun <T> attempt(what: String, action: () -> T): TagResult<T> = try {
        TagResult.Ok(action())
    } catch (e: MusicCoreException) {
        Log.e(TAG, "Couldn't $what", e)
        TagResult.Failed(e.status)
    }

    private companion object {
        const val TAG = "kithsong"
    }
}
