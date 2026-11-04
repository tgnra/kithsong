package ng.kithso.android.data

import android.util.Log
import androidx.compose.runtime.Immutable
import ng.kithso.core.AlbumFlags
import ng.kithso.core.AlbumTags
import ng.kithso.core.ArtistTags
import ng.kithso.core.CoreLibrary
import ng.kithso.core.MusicCoreException
import ng.kithso.core.ReleaseInfo
import ng.kithso.core.TrackTags
import ng.kithso.core.albumFlags
import ng.kithso.core.albumTags
import ng.kithso.core.artistTags
import ng.kithso.core.releaseTags
import ng.kithso.core.searchReleases
import ng.kithso.core.setAlbumFlags
import ng.kithso.core.setAlbumTags
import ng.kithso.core.setArtistTags
import ng.kithso.core.setTrackTags
import ng.kithso.core.trackTags
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.withContext
import java.io.File

/** What the tag editor opens on: an album, artist or track. */
@Immutable
data class TagTarget(val kind: Kind, val uid: String) {
    enum class Kind { Album, Artist, Track }
}

/** What the tag editor edits, as the core has it: an album's flags go with its tags. */
@Immutable
sealed interface TagValues {
    data class Album(val tags: AlbumTags, val flags: AlbumFlags) : TagValues
    data class Artist(val tags: ArtistTags) : TagValues
    data class Track(val tags: TrackTags) : TagValues
}

/** A [value], or the core's [Failed.status], for [ng.kithso.core.CoreEdit] to tell apart and put in words. */
sealed interface TagResult<out T> {
    data class Ok<T>(val value: T) : TagResult<T>
    data class Failed(val status: Int) : TagResult<Nothing>
}

/** How a save went: [saved] is what the library has now, and [status] the failure, 0 if there was none. */
@Immutable
data class TagSave(val saved: TagValues, val status: Int)

/**
 * Editing an album's, artist's or track's tags and an album's flags, and matching an album to
 * another MusicBrainz release, for a [LibrarySource] that can ([CoreTagEditor]; the made-up
 * library can't, and the UI then offers no Edit). What an edit does, to rows and files, is
 * all the core's (musiccore/edit.h).
 */
interface TagEditor {
    suspend fun read(target: TagTarget): TagResult<TagValues>

    /**
     * Saves what changed from [was] to [now], the tags, then an album's flags. A failure can
     * leave the tags saved and not the flags, which [TagSave.saved] says, for a second try
     * to leave them out. It's seen through, and the library read again, even if the caller goes.
     */
    suspend fun save(target: TagTarget, was: TagValues, now: TagValues): TagSave

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
 */
class CoreTagEditor(
    private val dir: File,
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
                TagTarget.Kind.Album -> TagValues.Album(lib.albumTags(target.uid), lib.albumFlags(target.uid))
                TagTarget.Kind.Artist -> TagValues.Artist(lib.artistTags(target.uid))
                TagTarget.Kind.Track -> TagValues.Track(lib.trackTags(target.uid))
            }
        }
    }

    // In [scope], so it's seen through and the library read again even if the screen goes away.
    override suspend fun save(target: TagTarget, was: TagValues, now: TagValues): TagSave = scope.async(saving) {
        var saved = was
        var status = 0
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
                        saved = now
                    }
                }
                was is TagValues.Artist && now is TagValues.Artist -> {
                    lib.setArtistTags(target.uid, now.tags)
                    saved = now
                }
                was is TagValues.Track && now is TagValues.Track -> {
                    lib.setTrackTags(target.uid, now.tags)
                    saved = now
                }
            }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't save $target", e)
            status = e.status
        }
        if (saved != was) changed()
        TagSave(saved, status)
    }.await()

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
