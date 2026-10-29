package ng.kithso.android.data

import android.content.SharedPreferences
import android.os.SystemClock
import android.util.Log
import ng.kithso.core.AlbumInfo
import ng.kithso.core.CoreImport
import ng.kithso.core.CoreLibrary
import ng.kithso.core.CoreSync
import ng.kithso.core.DeleteKind
import ng.kithso.core.DeviceKind
import ng.kithso.core.ItemKind
import ng.kithso.core.MusicCoreException
import ng.kithso.core.NativeCore
import ng.kithso.core.SearchKind
import ng.kithso.core.TrackInfo
import ng.kithso.core.addToAlbumGroup
import ng.kithso.core.albumGroupAlbums
import ng.kithso.core.albumGroupArt
import ng.kithso.core.appendToPlaylist
import ng.kithso.core.createAlbumGroup
import ng.kithso.core.createPlaylist
import ng.kithso.core.deleteAlbumGroup
import ng.kithso.core.deletePermanently
import ng.kithso.core.deletePlaylist
import ng.kithso.core.deletePreview
import ng.kithso.core.deleteTakesAny
import ng.kithso.core.moveInAlbumGroup
import ng.kithso.core.movePlaylistEntry
import ng.kithso.core.playlistEntries
import ng.kithso.core.removeFromAlbumGroup
import ng.kithso.core.removePlaylistEntry
import ng.kithso.core.renameAlbumGroup
import ng.kithso.core.renamePlaylist
import ng.kithso.core.artistAlbums
import ng.kithso.core.artistTracks
import ng.kithso.core.previewSmartTracks
import ng.kithso.core.clearRecents
import ng.kithso.core.recents
import ng.kithso.core.recordOpened
import ng.kithso.core.removeRecent
import ng.kithso.core.reshuffle
import ng.kithso.core.search
import ng.kithso.core.searchPrepare
import ng.kithso.core.smartAlbums
import ng.kithso.core.smartArt
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/**
 * The library as musiccore has it, in library folder [dir]. A library handle isn't
 * thread-safe, so every core call runs on one background thread ([core]); [library] is read
 * whole when this is created and again on [refresh], which is cheap enough to do after every
 * sync: one query for the albums, one for the tracks on this phone (for the artists), and
 * one per playlist and album group.
 *
 * Recents are the core's (mc_library_list_recents): what was last started playing, on any
 * device. Recents kept here before were in [prefs]; they're handed to the core once. They can
 * be taken out one by one or all at once ([RecentsEditor]).
 *
 * Sync brings the whole library's data, but only some of its files may be on this phone, so
 * only what has a file here is listed: albums with a folder here, tracks with a file,
 * artists with such an album or track, album groups with such an album, and playlists and
 * smart lists with at least one such track or album (the core's MC_LIST_PLAYABLE).
 * The sync profile editor's choices ([profileChoices]) still list every playlist.
 * Albums and tracks with no artist get [unknownArtist], looked up on every read so that it
 * follows the app's language.
 *
 * A playlist's or album group's picture is the core's (CoreLibrary.playlistArt,
 * albumGroupArt, and smartArt for smart ones), cached in [artDir]. Making one decodes four
 * covers, so after each read the pictures are looked up on a thread and library connection
 * of their own ([art]), and [library] is updated with those that changed. Meanwhile each
 * shows the picture it last had, kept in [prefs] across launches.
 *
 * Manual playlists and album groups are edited here too ([PlaylistEditor]), and albums,
 * artists and tracks deleted ([LibraryDeleter]), on [core], each change followed by a
 * [refresh].
 */
class CoreLibrarySource(
    private val dir: File,
    private val artDir: File,
    private val prefs: SharedPreferences,
    private val scope: CoroutineScope,
    private val unknownArtist: () -> String,
) : LibrarySource, PlaylistEditor, LibraryDeleter, RecentsEditor {
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
        handOverRecents()
        state.value = contents.library(recents())
        Log.i(TAG, "Read ${contents.albums.size} albums in ${SystemClock.elapsedRealtime() - start} ms")
        try {
            lib?.searchPrepare()
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't prepare search", e)
        }
        lookUpArt(contents.playlists.keys + contents.albumGroups.keys, ++artLookups)
    }

    // On [core]. Looks up the pictures of playlists and album groups [uids] on [art], then puts
    // those that changed in [contents], unless another read has started a lookup since.
    private fun lookUpArt(uids: Set<String>, lookup: Int) {
        if (uids.isEmpty()) return
        val groups = contents.albumGroups.keys
        val smart = contents.smart
        scope.launch(art) {
            val start = SystemClock.elapsedRealtime()
            val found = uids.associateWith { uid -> pictureOf(uid, uid in groups, uid in smart) }
            Log.i(TAG, "Looked up ${uids.size} pictures in ${SystemClock.elapsedRealtime() - start} ms")
            withContext(core) {
                if (lookup != artLookups) return@withContext
                saveArt(savedArt().filterKeys { it in contents.playlists || it in contents.albumGroups } + found)
                fun picture(uid: String, name: String, old: ArtRef): ArtRef {
                    val file = found[uid] ?: return old
                    val picture = artRef(uid, name, file.takeIf { it.path.isNotEmpty() })
                    return if (picture == old) old else picture
                }
                val playlists = contents.playlists.mapValues { (uid, p) -> p.copy(art = picture(uid, p.name, p.art)) }
                val albumGroups = contents.albumGroups.mapValues { (uid, g) -> g.copy(art = picture(uid, g.name, g.art)) }
                if (playlists == contents.playlists && albumGroups == contents.albumGroups) return@withContext
                contents = contents.copy(playlists = playlists, albumGroups = albumGroups)
                state.value = contents.library(recents())
            }
        }
    }

    // On [art]. [uid]'s picture, a playlist's or if [group] an album group's, a smart one's if
    // [smart], or an empty path for none (the placeholder).
    private fun pictureOf(uid: String, group: Boolean, smart: Boolean): File {
        val lib = artLib ?: try {
            CoreLibrary(dir.path).also { artLib = it }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't open the library in $dir for pictures", e)
            return File("")
        }
        return try {
            val path = when {
                smart -> lib.smartArt(uid, artDir.path)
                group -> lib.albumGroupArt(uid, artDir.path)
                else -> lib.playlistArt(uid, artDir.path)
            }
            File(path.orEmpty())
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't make $uid's picture", e)
            File("")
        }
    }

    // On [art]. Clears out the pictures of playlists and album groups (smart ones too) that have gone.
    private fun pruneArt() {
        try {
            CoreLibrary(dir.path).use { it.playlistArtPrune(artDir.path) }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't prune pictures in $artDir", e)
        }
    }

    // The playlists' and album groups' pictures as last looked up: lines of "uid:path", with an
    // empty path for none. On [core].
    private fun savedArt(): Map<String, File> =
        prefs.getString(KEY_PLAYLIST_ART, null).orEmpty().lines().filter { ':' in it }
            .associate { it.substringBefore(':') to File(it.substringAfter(':')) }

    private fun saveArt(art: Map<String, File>) {
        prefs.edit().putString(KEY_PLAYLIST_ART, art.entries.joinToString("\n") { (uid, file) -> "$uid:${file.path}" }).apply()
    }

    /** Starts importing [paths] into the library; see [CoreImport]. Close it when done. */
    suspend fun startImport(
        paths: List<String>,
        options: CoreImport.Options,
        listener: (CoreImport.Event, Int) -> Unit,
    ): CoreImport? = withContext(core) { open()?.let { CoreImport(it, paths, options, listener) } }

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
                // The albums it's on, its own first, then every track credited to it.
                CollectionRef.Kind.Artist -> contents.artists[ref.uid]?.let { artist ->
                    val albums = lib.artistAlbums(artist.uid).mapNotNull { contents.albums[it.uid] }
                    val tracks = lib.artistTracks(artist.uid).here().map { covers.track(it) }
                    TrackList(artist.name, artist.art, tracks, albumCount = albums.size, albums = albums)
                }
                CollectionRef.Kind.Playlist -> contents.playlists[ref.uid]?.let { playlist ->
                    val tracks = if (ref.uid in contents.smart) {
                        lib.smartTracks(ref.uid).here().map { covers.track(it) }
                    } else {
                        // With their entries, for editing.
                        lib.playlistTracks(ref.uid).zip(lib.playlistEntries(ref.uid))
                            .filter { (track, _) -> track.path != null }
                            .map { (track, entry) -> covers.track(track).copy(entryUid = entry) }
                    }
                    TrackList(playlist.name, playlist.art, tracks, canReshuffle = ref.uid in contents.random)
                }
                // Its albums, and their tracks one album after another to play it.
                CollectionRef.Kind.AlbumGroup -> contents.albumGroups[ref.uid]?.let { group ->
                    val infos = if (ref.uid in contents.smart) lib.smartAlbums(ref.uid) else lib.albumGroupAlbums(ref.uid)
                    val albums = infos.mapNotNull { contents.albums[it.uid] }
                    val tracks = albums.flatMap { album -> lib.albumTracks(album.uid).here().map { covers.track(it) } }
                    TrackList(
                        group.name, group.art, tracks,
                        albumCount = albums.size, albums = albums, canReshuffle = ref.uid in contents.random,
                    )
                }
            }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't list $ref", e)
            null
        }
    }

    override fun notePlayed(ref: CollectionRef) {
        scope.launch(core) {
            val lib = open() ?: return@launch
            try {
                lib.recordOpened(ref.kind.itemKind, ref.uid)
            } catch (e: MusicCoreException) {
                Log.e(TAG, "Couldn't add $ref to the recents", e)
                return@launch
            }
            state.value = state.value?.copy(recents = recents())
            onChanged()
        }
    }

    override fun removeRecent(ref: CollectionRef) = changeRecents("take $ref out of the recents") {
        it.removeRecent(ref.uid)
    }

    override fun clearRecents() = changeRecents("clear the recents") { it.clearRecents() }

    private fun changeRecents(what: String, change: (CoreLibrary) -> Unit) {
        scope.launch(core) {
            val lib = open() ?: return@launch
            try {
                change(lib)
            } catch (e: MusicCoreException) {
                Log.e(TAG, "Couldn't $what", e)
                return@launch
            }
            state.value = state.value?.copy(recents = recents())
            onChanged()
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
            onChanged()
        }
    }

    // --- PlaylistEditor ---

    private val editCount = MutableStateFlow(0)
    override val edits: StateFlow<Int> = editCount
    private val failed = MutableSharedFlow<EditFailure>(extraBufferCapacity = 8)
    override val failures: SharedFlow<EditFailure> = failed

    override suspend fun playlistChoices(): List<PlaylistChoice>? = withContext(core) {
        try {
            open()?.playlists()?.map { PlaylistChoice(it.uid, it.name) }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't list the playlists", e)
            null
        }
    }

    override suspend fun isEditable(playlistUid: String): Boolean = withContext(core) {
        playlistUid in contents.playlists && playlistUid !in contents.smart
    }

    override fun addToPlaylist(playlistUid: String, what: Addition) =
        edit(EditFailure.PlaylistAdd, "add to playlist $playlistUid") { it.appendToPlaylist(playlistUid, trackUids(it, what)) }

    override fun addToNewPlaylist(name: String, what: Addition) = edit(EditFailure.PlaylistCreate, "make playlist $name") {
        val uid = it.createPlaylist(name)
        it.appendToPlaylist(uid, trackUids(it, what))
    }

    override fun renamePlaylist(playlistUid: String, name: String) =
        edit(EditFailure.PlaylistRename, "rename playlist $playlistUid") { it.renamePlaylist(playlistUid, name) }

    override fun deletePlaylist(playlistUid: String) =
        edit(EditFailure.PlaylistDelete, "delete playlist $playlistUid") { it.deletePlaylist(playlistUid) }

    override fun removeEntry(entryUid: String) =
        edit(EditFailure.PlaylistRemove, "remove playlist entry $entryUid") { it.removePlaylistEntry(entryUid) }

    override fun moveEntry(entryUid: String, afterEntryUid: String?) =
        edit(EditFailure.PlaylistMove, "move playlist entry $entryUid") { it.movePlaylistEntry(entryUid, afterEntryUid) }

    override suspend fun albumGroupChoices(): List<PlaylistChoice>? = withContext(core) {
        try {
            open()?.albumGroups()?.map { PlaylistChoice(it.uid, it.name) }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't list the album groups", e)
            null
        }
    }

    override suspend fun isEditableGroup(groupUid: String): Boolean = withContext(core) {
        groupUid in contents.albumGroups && groupUid !in contents.smart
    }

    override fun addToAlbumGroup(groupUid: String, albumUid: String) =
        edit(EditFailure.GroupAdd, "add album $albumUid to album group $groupUid") { it.addToAlbumGroup(groupUid, albumUid) }

    override fun addToNewAlbumGroup(name: String, albumUid: String) = edit(EditFailure.GroupCreate, "make album group $name") {
        it.addToAlbumGroup(it.createAlbumGroup(name), albumUid)
    }

    override fun renameAlbumGroup(groupUid: String, name: String) =
        edit(EditFailure.GroupRename, "rename album group $groupUid") { it.renameAlbumGroup(groupUid, name) }

    override fun deleteAlbumGroup(groupUid: String) =
        edit(EditFailure.GroupDelete, "delete album group $groupUid") { it.deleteAlbumGroup(groupUid) }

    override fun removeFromAlbumGroup(groupUid: String, albumUid: String) =
        edit(EditFailure.GroupRemove, "remove album $albumUid from album group $groupUid") { it.removeFromAlbumGroup(groupUid, albumUid) }

    override fun moveInAlbumGroup(groupUid: String, albumUid: String, afterAlbumUid: String?) =
        edit(EditFailure.GroupMove, "move album $albumUid in album group $groupUid") { it.moveInAlbumGroup(groupUid, albumUid, afterAlbumUid) }

    // On [core]. The tracks [what] adds: an album's that have a file here, as the album lists
    // them, since that's what this phone shows of it.
    private fun trackUids(lib: CoreLibrary, what: Addition): List<String> = when (what) {
        is Addition.Tracks -> what.uids
        is Addition.Album -> lib.albumTracks(what.uid).here().map { it.uid }
    }

    // Makes a change on [core], then reads the library again and passes the change on to sync.
    // A failure is reported as [failure].
    private fun edit(failure: EditFailure, what: String, change: (CoreLibrary) -> Unit) {
        scope.launch(core) {
            val lib = open() ?: run {
                failed.tryEmit(failure)
                return@launch
            }
            try {
                change(lib)
            } catch (e: MusicCoreException) {
                Log.e(TAG, "Couldn't $what", e)
                failed.tryEmit(failure)
            }
            refresh().join()
            editCount.value++
            onChanged()
        }
    }

    // --- LibraryDeleter ---

    override suspend fun deleteSize(what: Deletion): DeleteSize? = withContext(core) {
        try {
            open()?.deletePreview(what.kind.core, what.uid)?.let { DeleteSize(it.tracks, it.files) }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't find out what deleting $what takes", e)
            null
        }
    }

    override suspend fun takesAny(what: Deletion, trackUids: List<String>): Boolean = withContext(core) {
        try {
            open()?.deleteTakesAny(what.kind.core, what.uid, trackUids) == true
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't find out what deleting $what takes", e)
            false
        }
    }

    // In [scope], so it's seen through and the library read again even if the screen that
    // asked goes away.
    override suspend fun delete(what: Deletion): DeleteOutcome = scope.async(core) {
        val lib = open() ?: return@async DeleteOutcome.Failed
        val outcome = try {
            val result = lib.deletePermanently(what.kind.core, what.uid)
            Log.i(TAG, "Deleted $what: ${result.tracksDeleted} tracks, ${result.filesRemoved} files, ${result.tracksKept} kept")
            if (result.tracksKept > 0) DeleteOutcome.Kept(result.tracksKept) else DeleteOutcome.Deleted
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't delete $what", e)
            DeleteOutcome.Failed
        }
        // Even after a failure: some files may have gone.
        refresh().join()
        editCount.value++
        onChanged()
        outcome
    }.await()

    override suspend fun search(query: String): SearchResults = withContext(core) {
        val hits = try {
            open()?.search(query, SEARCH_LIMIT).orEmpty()
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't search for \"$query\"", e)
            emptyList()
        }
        // The index can be a little behind the library, and knows what isn't on this phone:
        // only what's listed here is shown, as it was last read.
        val c = contents
        val covers = Covers()
        val order = ArrayList<SearchResults.Kind>()
        val artists = ArrayList<Artist>()
        val albums = ArrayList<Album>()
        val tracks = ArrayList<FoundTrack>()
        val playlists = ArrayList<Playlist>()
        val albumGroups = ArrayList<AlbumGroup>()
        for (hit in hits) {
            val kind = when (hit.kind) {
                SearchKind.Artist -> c.artists[hit.uid]?.let(artists::add)?.let { SearchResults.Kind.Artists }
                SearchKind.Album -> c.albums[hit.uid]?.let(albums::add)?.let { SearchResults.Kind.Albums }
                SearchKind.Track -> {
                    // A track is found by its album, whose art and tracks are here.
                    val album = hit.albumUid?.let { c.albums[it] } ?: continue
                    tracks += FoundTrack(
                        hit.uid, hit.name, hit.artist ?: unknownArtist(), hit.album.orEmpty(), album.uid,
                        covers.found(album),
                    )
                    SearchResults.Kind.Tracks
                }
                SearchKind.Playlist -> c.playlists[hit.uid]?.let(playlists::add)?.let { SearchResults.Kind.Playlists }
                SearchKind.AlbumGroup -> c.albumGroups[hit.uid]?.let(albumGroups::add)?.let { SearchResults.Kind.AlbumGroups }
            } ?: continue
            if (kind !in order) order += kind
        }
        SearchResults(order, artists, albums, tracks, playlists, albumGroups)
    }

    override suspend fun reshuffle(ref: CollectionRef) {
        withContext(core) {
            val lib = open() ?: return@withContext
            try {
                lib.reshuffle(ref.uid)
            } catch (e: MusicCoreException) {
                Log.e(TAG, "Couldn't reshuffle $ref", e)
                return@withContext
            }
            onChanged()
        }
        // A smart list's picture is made from what it holds, which may have changed.
        refresh().join()
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

        // The core's artists with an album or track here. Neither an album nor a track gives its
        // artist's uid, so they're matched by name: two artists of the same name share both.
        val albumsBy = albums.groupBy { it.artist }
        val tracksBy = lib.previewSmartTracks(ON_DEVICE_RULE).groupingBy { it.artist ?: unknownArtist() }.eachCount()
        val artists = lib.artists().mapNotNull { info ->
            val own = albumsBy[info.name].orEmpty()
            val trackCount = tracksBy[info.name] ?: 0
            if (own.isEmpty() && trackCount == 0) return@mapNotNull null
            // A cover file before an audio file that may have no picture in it.
            val cover = (own.firstOrNull { album -> album.art.file?.let { File(it).extension.lowercase() in COVER_EXTENSIONS } == true }
                ?: own.firstOrNull { it.art.file != null })?.art
            Artist(info.uid, info.name, own.size, cover?.copy(uid = info.uid, label = info.name) ?: ArtRef(info.uid, info.name), trackCount)
        }

        // Smart playlists sit among the others, and smart album groups among the manual ones.
        // Only those with a track or album to play here, which also keeps the rest out of
        // Recents. Each shows the picture it last had until [lookUpArt] finds it again.
        val here = albums.associateBy { it.uid }
        val smart = lib.smartLists(playableOnly = true)
        val known = savedArt()
        fun knownArt(uid: String, name: String) = artRef(uid, name, known[uid]?.takeIf { it.path.isNotEmpty() && it.isFile })
        val playlists = lib.playlists(playableOnly = true).map { info ->
            Playlist(info.uid, info.name, info.playableCount, knownArt(info.uid, info.name))
        } + smart.filter { !it.isAlbums }.map { info ->
            val tracks = lib.smartTracks(info.uid).here()
            Playlist(info.uid, info.name, tracks.size, knownArt(info.uid, info.name))
        }
        val albumGroups = lib.albumGroups().mapNotNull { info ->
            val count = lib.albumGroupAlbums(info.uid).count { it.uid in here }
            if (count == 0) null else AlbumGroup(info.uid, info.name, count, knownArt(info.uid, info.name))
        } + smart.filter { it.isAlbums }.map { info ->
            val count = lib.smartAlbums(info.uid).count { it.uid in here }
            AlbumGroup(info.uid, info.name, count, knownArt(info.uid, info.name))
        }

        return Contents(
            here,
            artists.associateBy { it.uid },
            playlists.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name }).associateBy { it.uid },
            albumGroups.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name }).associateBy { it.uid },
            smart.mapTo(HashSet()) { it.uid },
            smart.filter { it.isRandom }.mapTo(HashSet()) { it.uid },
        )
    }

    // On [core]. Recents used to be kept here, as lines of "kind:time:uid" (or "kind:uid")
    // in [prefs], newest first; they go to the core oldest first, so the newest ends up first.
    // An artist's uid was its name then, which the core doesn't know, so those are dropped.
    private fun handOverRecents() {
        val saved = prefs.getString(KEY_RECENTS, null) ?: return
        val lib = open() ?: return
        for (line in saved.lines().reversed()) {
            val kind = CollectionRef.Kind.entries.firstOrNull { it.name == line.substringBefore(':') } ?: continue
            if (kind == CollectionRef.Kind.Artist) continue
            val rest = line.substringAfter(':')
            val uid = if (rest.substringBefore(':').toLongOrNull() != null) rest.substringAfter(':') else rest
            try {
                lib.recordOpened(kind.itemKind, uid)
            } catch (e: MusicCoreException) {
                Log.i(TAG, "Left $kind $uid out of the recents", e)
            }
        }
        prefs.edit().remove(KEY_RECENTS).apply()
        onChanged()
    }

    // On [core]. The core's recents that are on this phone, newest first. Its list leaves out
    // playlists with nothing here, but not albums or artists, so it's asked for more than are
    // shown.
    private fun recents(): List<Recent> = try {
        open()?.recents(MAX_RECENTS * 4, playableOnly = true).orEmpty()
            .mapNotNull { recent(CollectionRef(it.kind.refKind, it.uid)) }
            .take(MAX_RECENTS)
    } catch (e: MusicCoreException) {
        Log.e(TAG, "Couldn't list the recents", e)
        emptyList()
    }

    private fun recent(ref: CollectionRef): Recent? = when (ref.kind) {
        CollectionRef.Kind.Album -> contents.albums[ref.uid]?.let { Recent(ref, it.title, it.artist, it.art) }
        CollectionRef.Kind.Artist -> contents.artists[ref.uid]?.let { Recent(ref, it.name, null, it.art) }
        CollectionRef.Kind.Playlist -> contents.playlists[ref.uid]?.let { Recent(ref, it.name, null, it.art) }
        CollectionRef.Kind.AlbumGroup -> contents.albumGroups[ref.uid]?.let { Recent(ref, it.name, null, it.art) }
    }

    // The tracks with a file on this phone.
    private fun List<TrackInfo>.here() = filter { it.path != null }

    /**
     * What the last read found, for building [Library] and looking things up by uid. [smart]
     * are the smart lists among the playlists and album groups, and [random] those of them
     * ordered by random.
     */
    private data class Contents(
        val albums: Map<String, Album>,
        val artists: Map<String, Artist>,
        val playlists: Map<String, Playlist>,
        val albumGroups: Map<String, AlbumGroup>,
        val smart: Set<String>,
        val random: Set<String>,
    ) {
        fun library(recents: List<Recent>) = Library(
            recents,
            playlists.values.toList(),
            albums.values.toList(),
            artists.values.toList(),
            albumGroups.values.toList(),
        )

        companion object {
            val Empty = Contents(emptyMap(), emptyMap(), emptyMap(), emptyMap(), emptySet(), emptySet())
        }
    }

    /**
     * Makes [ArtRef]s for covers in album folders, as the core finds them ([NativeCore.coverFile]).
     * Each folder is looked at once. Inner, for [unknownArtist].
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

        /** A found track's art: its album's, keyed by the album's title as a track's is. */
        fun found(album: Album): ArtRef = album.art.copy(uid = album.title, label = album.title)

        private fun ref(uid: String, label: String, file: File?) = artRef(uid, label, file)

        private fun coverIn(dir: String): File? = NativeCore.coverFile(dir)?.let(::File)
    }

    private companion object {
        const val TAG = "kithsong"
        const val KEY_RECENTS = "recents" // only read once, to hand them to the core
        const val KEY_PLAYLIST_ART = "playlist_art"
        const val MAX_RECENTS = 8
        const val SEARCH_LIMIT = 20 // of each kind, as the desktops show
        const val ON_DEVICE_RULE = """{"v": 1, "match": {"field": "on_device", "op": "is", "value": true}}"""
        val COVER_EXTENSIONS = listOf("jpg", "jpeg", "png") // what mc_coverart_find() takes

        fun artRef(uid: String, label: String, file: File?) = ArtRef(uid, label, file?.path, file?.lastModified() ?: 0)

        val Deletion.Kind.core: DeleteKind
            get() = when (this) {
                Deletion.Kind.Track -> DeleteKind.Track
                Deletion.Kind.Album -> DeleteKind.Album
                Deletion.Kind.Artist -> DeleteKind.Artist
            }

        val CollectionRef.Kind.itemKind: ItemKind
            get() = when (this) {
                CollectionRef.Kind.Album -> ItemKind.Album
                CollectionRef.Kind.Artist -> ItemKind.Artist
                CollectionRef.Kind.Playlist -> ItemKind.Playlist
                CollectionRef.Kind.AlbumGroup -> ItemKind.AlbumGroup
            }

        val ItemKind.refKind: CollectionRef.Kind
            get() = when (this) {
                ItemKind.Album -> CollectionRef.Kind.Album
                ItemKind.Artist -> CollectionRef.Kind.Artist
                ItemKind.Playlist -> CollectionRef.Kind.Playlist
                ItemKind.AlbumGroup -> CollectionRef.Kind.AlbumGroup
            }
    }
}
