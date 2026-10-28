package app.musicapp.data

import android.content.SharedPreferences
import android.os.SystemClock
import android.util.Log
import app.musicapp.AlbumInfo
import app.musicapp.CoreLibrary
import app.musicapp.CoreSync
import app.musicapp.DeviceKind
import app.musicapp.MusicCoreException
import app.musicapp.TrackInfo
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * The library as musiccore has it, in library folder [dir]. A library handle isn't
 * thread-safe, so every core call runs on one background thread ([core]); [library] is read
 * whole when this is created and again on [refresh], which is cheap enough to do after every
 * sync: one query for the albums and one per playlist.
 *
 * Recents are the albums the core has the newest plays of ([recordPlay]; plays on other
 * devices arrive with sync), merged with what was last started here, kept in [prefs]: the core
 * doesn't know a track was played from a playlist or an artist, and a track only counts as a
 * play once it has played for a while, whereas Recents should show what was tapped at once.
 *
 * Sync brings the whole library's data, but only some of its files may be on this phone, so
 * only what has a file here is listed: albums with a folder here, tracks with a file, and
 * playlists and smart playlists with at least one such track (the core's MC_LIST_PLAYABLE).
 * The sync profile editor's choices ([profileChoices]) still list every playlist.
 * Albums and tracks with no artist get [unknownArtist], looked up on every read so that it
 * follows the app's language.
 *
 * A playlist's picture is the core's (CoreLibrary.playlistArt), cached in [artDir]. Making
 * one decodes four covers, so after each read the pictures are looked up on a thread and
 * library connection of their own ([art]), and [library] is updated with those that changed.
 * Meanwhile a playlist shows the picture it last had, kept in [prefs] across launches.
 */
class CoreLibrarySource(
    private val dir: File,
    private val artDir: File,
    private val prefs: SharedPreferences,
    private val scope: CoroutineScope,
    private val unknownArtist: () -> String,
) : LibrarySource {
    private val core = Dispatchers.IO.limitedParallelism(1)
    private val state = MutableStateFlow<Library?>(null)
    override val library: StateFlow<Library?> = state

    /** Called after a change made here, for sync to pass on soon. */
    var onChanged: () -> Unit = {}

    // Only touched on [core].
    private var lib: CoreLibrary? = null
    private var contents = Contents.Empty
    private var artLookups = 0 // bumped by each read, so only the latest read's lookup is used

    private val art = Dispatchers.IO.limitedParallelism(1)
    private var artLib: CoreLibrary? = null // only touched on [art]

    init {
        refresh()
        scope.launch(art) { pruneArt() }
    }

    /** Reads the library again, e.g. after a sync. */
    fun refresh(): Job = scope.launch(core) {
        val start = SystemClock.elapsedRealtime()
        contents = open()?.let { lib ->
            try {
                read(lib)
            } catch (e: MusicCoreException) {
                Log.e(TAG, "Couldn't read the library", e)
                null
            }
        } ?: Contents.Empty
        state.value = contents.library(recents())
        Log.i(TAG, "Read ${contents.albums.size} albums in ${SystemClock.elapsedRealtime() - start} ms")
        lookUpArt(contents.playlists.keys - contents.smart, ++artLookups)
    }

    // On [core]. Looks up the pictures of playlists [uids] on [art], then puts those that
    // changed in [contents], unless another read has started a lookup since.
    private fun lookUpArt(uids: Set<String>, lookup: Int) {
        if (uids.isEmpty()) return
        scope.launch(art) {
            val start = SystemClock.elapsedRealtime()
            val found = uids.associateWith { uid -> playlistArt(uid) }
            Log.i(TAG, "Looked up ${uids.size} playlists' pictures in ${SystemClock.elapsedRealtime() - start} ms")
            withContext(core) {
                if (lookup != artLookups) return@withContext
                saveArt(savedArt().filterKeys { it in contents.playlists } + found)
                val playlists = contents.playlists.mapValues { (uid, playlist) ->
                    val file = found[uid] ?: return@mapValues playlist
                    val picture = artRef(uid, playlist.name, file.takeIf { it.path.isNotEmpty() })
                    if (picture == playlist.art) playlist else playlist.copy(art = picture)
                }
                if (playlists == contents.playlists) return@withContext
                contents = Contents(contents.albums, contents.artists, playlists, contents.smart)
                state.value = contents.library(recents())
            }
        }
    }

    // On [art]. [uid]'s picture, or an empty path for none (the placeholder).
    private fun playlistArt(uid: String): File {
        val lib = artLib ?: try {
            CoreLibrary(dir.path).also { artLib = it }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't open the library in $dir for playlists' pictures", e)
            return File("")
        }
        return try {
            File(lib.playlistArt(uid, artDir.path).orEmpty())
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't make playlist $uid's picture", e)
            File("")
        }
    }

    // On [art]. Clears out the pictures of playlists that have gone.
    private fun pruneArt() {
        try {
            CoreLibrary(dir.path).use { it.playlistArtPrune(artDir.path) }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't prune playlists' pictures in $artDir", e)
        }
    }

    // The playlists' pictures as last looked up: lines of "uid:path", with an empty path for
    // none. On [core].
    private fun savedArt(): Map<String, File> =
        prefs.getString(KEY_PLAYLIST_ART, null).orEmpty().lines().filter { ':' in it }
            .associate { it.substringBefore(':') to File(it.substringAfter(':')) }

    private fun saveArt(art: Map<String, File>) {
        prefs.edit().putString(KEY_PLAYLIST_ART, art.entries.joinToString("\n") { (uid, file) -> "$uid:${file.path}" }).apply()
    }

    /** Opens this phone's sync state in [configDir] for the library; see [CoreSync]. */
    suspend fun openSync(configDir: String, deviceName: String, listener: (CoreSync.Event) -> Unit): CoreSync? =
        withContext(core) { open()?.let { CoreSync(it, configDir, deviceName, DeviceKind.Phone, listener) } }

    /**
     * What a sync profile can select (see [ProfileChoices]); smart playlists and album groups
     * are left out, as profiles only take manual ones. Throws MusicCoreException.
     */
    suspend fun profileChoices(): ProfileChoices = withContext(core) {
        val lib = open() ?: throw MusicCoreException(CoreSync.ERR_NOT_FOUND)
        ProfileChoices(
            albums = lib.albums().map { ProfileChoice(it.uid, it.title, it.artist ?: unknownArtist(), it.year) },
            artists = lib.artists().map { ProfileChoice(it.uid, it.name, albumCount = it.albumCount) },
            playlists = lib.playlists().map { ProfileChoice(it.uid, it.name) },
            albumGroups = lib.albumGroups().map { ProfileChoice(it.uid, it.name, albumCount = it.albumCount) },
        )
    }

    override suspend fun trackList(ref: CollectionRef): TrackList? = withContext(core) {
        val lib = open() ?: return@withContext null
        val covers = Covers()
        try {
            when (ref.kind) {
                CollectionRef.Kind.Album -> contents.albums[ref.uid]?.let { album ->
                    TrackList(album.title, album.art, lib.albumTracks(album.uid).here().map { covers.track(it) }, album.artist, album.year)
                }
                CollectionRef.Kind.Artist -> contents.artists[ref.uid]?.let { (artist, albums) ->
                    val tracks = albums.flatMap { album -> lib.albumTracks(album.uid).here().map { covers.track(it) } }
                    TrackList(artist.name, artist.art, tracks, albumCount = albums.size)
                }
                CollectionRef.Kind.Playlist -> contents.playlists[ref.uid]?.let { playlist ->
                    val tracks = (if (ref.uid in contents.smart) lib.smartTracks(ref.uid) else lib.playlistTracks(ref.uid)).here()
                    TrackList(playlist.name, playlist.art, tracks.map { covers.track(it) })
                }
            }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't list $ref", e)
            null
        }
    }

    override fun notePlayed(ref: CollectionRef) {
        val now = System.currentTimeMillis()
        scope.launch(core) {
            val saved = (listOf(ref to now) + savedRecents().filter { it.first != ref }).take(MAX_RECENTS)
            prefs.edit().putString(KEY_RECENTS, saved.joinToString("\n") { (ref, time) -> "${ref.kind.name}:$time:${ref.uid}" }).apply()
            state.value = state.value?.copy(recents = recents())
        }
    }

    override fun recordPlay(trackUid: String) {
        scope.launch(core) {
            val lib = open() ?: return@launch
            try {
                lib.recordPlay(trackUid)
            } catch (e: MusicCoreException) {
                Log.e(TAG, "Couldn't record a play of $trackUid", e)
                return@launch
            }
            state.value = state.value?.copy(recents = recents())
            onChanged()
        }
    }

    // On [core]. Null if the library can't be opened.
    private fun open(): CoreLibrary? {
        lib?.let { return it }
        return try {
            CoreLibrary(dir.path).also { lib = it }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't open the library in $dir", e)
            null
        }
    }

    private fun read(lib: CoreLibrary): Contents {
        val covers = Covers()
        val albums = lib.albums().filter { it.folder != null }.map { info -> Album(info.uid, info.title, info.artist ?: unknownArtist(), info.year, covers.album(info)) }

        // The core lists albums by artist, so the artists come out in order too.
        val artists = albums.groupBy { it.artist }.map { (name, albums) ->
            // The uid is the name: the core has no artist list yet, and albums give a name.
            // A cover file before an audio file that may have no picture in it.
            val cover = (albums.firstOrNull { album -> album.art.file?.let { File(it).name in COVER_NAMES } == true }
                ?: albums.firstOrNull { it.art.file != null })?.art
            Artist(name, name, albums.size, cover?.copy(uid = name, label = name) ?: ArtRef(name, name)) to albums
        }

        // Smart playlists sit among the others; smart album groups have no place on this screen.
        // Only those with a track to play here, which also keeps the rest out of Recents.
        // A manual playlist shows the picture it last had until [lookUpArt] finds it again.
        val smart = lib.smartLists(playableOnly = true).filter { !it.isAlbums }
        val known = savedArt()
        val playlists = lib.playlists(playableOnly = true).map { info ->
            val tracks = lib.playlistTracks(info.uid).here()
            val file = known[info.uid]?.takeIf { it.path.isNotEmpty() && it.isFile }
            Playlist(info.uid, info.name, tracks.size, artRef(info.uid, info.name, file))
        } + smart.map { info ->
            val tracks = lib.smartTracks(info.uid).here()
            Playlist(info.uid, info.name, tracks.size, covers.playlist(info.uid, info.name, tracks))
        }

        return Contents(
            albums.associateBy { it.uid },
            artists.associateBy { it.first.uid },
            playlists.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name }).associateBy { it.uid },
            smart.mapTo(HashSet()) { it.uid },
        )
    }

    // What was last started here, newest first, with when: lines of "kind:time:uid". Lines
    // from before there were times are "kind:uid", and an artist's uid is its name, which
    // may hold a colon.
    private fun savedRecents(): List<Pair<CollectionRef, Long>> =
        prefs.getString(KEY_RECENTS, null).orEmpty().lines().mapNotNull { line ->
            val kind = CollectionRef.Kind.entries.firstOrNull { it.name == line.substringBefore(':') }
                ?: return@mapNotNull null
            val rest = line.substringAfter(':')
            val time = rest.substringBefore(':').toLongOrNull()
            CollectionRef(kind, if (time != null) rest.substringAfter(':') else rest) to (time ?: 0)
        }

    // On [core]. The albums with the newest plays, newest first.
    private fun playedAlbums(): List<String> = try {
        open()?.previewSmartAlbums(RECENT_ALBUMS_RULE)?.map { it.uid }.orEmpty()
    } catch (e: MusicCoreException) {
        Log.e(TAG, "Couldn't list recently played albums", e)
        emptyList()
    }

    // On [core]. Recently played things that are still in the library. The core gives an
    // order but no times, so a played album takes its time from when it was started here,
    // else from the album before it, and then everything is sorted by time.
    private fun recents(): List<Recent> {
        val saved = savedRecents()
        val times = saved.toMap()
        var time = Long.MAX_VALUE
        val played = playedAlbums().map { uid ->
            val ref = CollectionRef(CollectionRef.Kind.Album, uid)
            time = times[ref] ?: time
            ref to time
        }
        val refs = (played + saved).sortedByDescending { it.second }.map { it.first }.distinct()
        return refs.mapNotNull(::recent).take(MAX_RECENTS)
    }

    private fun recent(ref: CollectionRef): Recent? = when (ref.kind) {
        CollectionRef.Kind.Album -> contents.albums[ref.uid]?.let { Recent(ref, it.title, it.artist, it.art) }
        CollectionRef.Kind.Artist -> contents.artists[ref.uid]?.let { (artist, _) ->
            Recent(ref, artist.name, null, artist.art)
        }
        CollectionRef.Kind.Playlist -> contents.playlists[ref.uid]?.let { Recent(ref, it.name, null, it.art) }
    }

    // The tracks with a file on this phone.
    private fun List<TrackInfo>.here() = filter { it.path != null }

    /** What the last read found, for building [Library] and looking things up by uid. */
    private class Contents(
        val albums: Map<String, Album>,
        val artists: Map<String, Pair<Artist, List<Album>>>,
        val playlists: Map<String, Playlist>,
        val smart: Set<String>,
    ) {
        fun library(recents: List<Recent>) = Library(
            recents,
            playlists.values.toList(),
            albums.values.toList(),
            artists.values.map { it.first },
        )

        companion object {
            val Empty = Contents(emptyMap(), emptyMap(), emptyMap(), emptySet())
        }
    }

    /**
     * Makes [ArtRef]s for covers in album folders: cover.jpg or cover.png, as the core and the
     * desktop apps look for. Each folder is looked at once. Inner, for [unknownArtist].
     */
    private inner class Covers {
        private val found = HashMap<String, File?>()

        fun album(info: AlbumInfo): ArtRef {
            val folder = info.folder ?: return ArtRef(info.uid, info.title)
            // Embedded art is never shown: the cover file is the album's art.
            val file = if (info.hasArt) found.getOrPut(folder) { coverIn(folder) } else null
            return ref(info.uid, info.title, file)
        }

        // A track's album cover.
        fun track(info: TrackInfo): Track {
            val album = info.album.orEmpty()
            val file = info.coverDir?.let { dir -> found.getOrPut(dir) { coverIn(dir) } }
            return Track(
                uid = info.uid,
                title = info.title,
                artist = info.artist ?: unknownArtist(),
                album = album,
                trackNo = info.trackNo,
                durationMs = info.durationMs.toLong(),
                art = ref(album.ifEmpty { info.uid }, album.ifEmpty { info.title }, file),
                path = info.path,
            )
        }

        /** A smart playlist shows its first track's cover; the core makes manual ones' pictures. */
        fun playlist(uid: String, name: String, tracks: List<TrackInfo>): ArtRef {
            val first = tracks.firstOrNull { it.coverDir != null }
            val cover = first?.coverDir?.let { dir -> found.getOrPut(dir) { coverIn(dir) } }
            return ref(uid, name, cover)
        }

        private fun ref(uid: String, label: String, file: File?) = artRef(uid, label, file)

        private fun coverIn(dir: String): File? =
            COVER_NAMES.asSequence().map { File(dir, it) }.firstOrNull { it.isFile }
    }

    private companion object {
        const val TAG = "musicapp"
        const val KEY_RECENTS = "recents"
        const val KEY_PLAYLIST_ART = "playlist_art"
        const val MAX_RECENTS = 8
        const val RECENT_ALBUMS_RULE = """{"v": 1, "match": {"field": "last_played", "op": "not_empty"},
            "order": [{"field": "last_played", "dir": "desc"}], "limit": $MAX_RECENTS}"""
        val COVER_NAMES = listOf("cover.jpg", "cover.png")

        fun artRef(uid: String, label: String, file: File?) = ArtRef(uid, label, file?.path, file?.lastModified() ?: 0)
    }
}
