package ng.kithso.core

/**
 * An import session (mc_import): scans [paths] on a core-owned thread, matches what it finds
 * against MusicBrainz and copies it into [library], which must stay open until this is
 * closed. A path that is a .zip file is extracted first, into a folder of its own under
 * [Options.stagingDir] that the session deletes when it's closed. [listener] is called on the
 * import thread with an [Event] and its argument (see mc_import_event), possibly before the
 * constructor has returned, so it should hand events to another thread rather than call back
 * into this. Everything else may be called from any thread.
 */
class CoreImport(
    library: CoreLibrary,
    paths: List<String>,
    options: Options,
    private val listener: (Event, Int) -> Unit,
) : AutoCloseable {
    /** mc_import_options. */
    data class Options(
        /** Download front covers for albums that have none. */
        val fetchArt: Boolean = true,
        /** No MusicBrainz lookups: every item is imported with the tags it has. */
        val skipTagging: Boolean = false,
        /** Where zips are extracted; null for the system's temporary folder. */
        val stagingDir: String? = null,
    )

    /** In mc_import_event's order. [Offline]'s argument is 1 while MusicBrainz can't be reached. */
    enum class Event { Scanning, Scanned, ItemChanged, Idle, Offline }

    enum class State {
        Pending, Matching, Review, Writing, Done, Skipped, Failed;

        val isFinished: Boolean get() = this == Done || this == Skipped || this == Failed
        val isBusy: Boolean get() = this == Pending || this == Matching || this == Writing
    }

    enum class Kind { Album, Single }

    /** mc_import_outcome: what writing an item did, beyond its state. */
    enum class Outcome { None, Replaced, Added, Duplicate, Moved }

    /** A snapshot of an item (mc_import_item, without its files). */
    data class ItemInfo(
        val kind: Kind,
        val state: State,
        val fileCount: Int,
        val artist: String,
        val album: String,
        val folder: String,
        /** Why it failed or needs review, in the core's language, or null. */
        val error: String?,
        /** Its candidates include an extended search's. */
        val searchedWider: Boolean,
        val outcome: Outcome,
        /** The match it was written with; -1 for its existing tags, or not yet decided. */
        val chosen: Int,
        val matchCount: Int,
    )

    /** One of an item's candidates on MusicBrainz (mc_import_match). Empty strings when unknown. */
    data class Match(
        val album: String,
        val albumArtist: String,
        /** The MusicBrainz release's title, e.g. a single's own. */
        val release: String,
        val year: Int,
        /** 0 to 1. */
        val confidence: Double,
        val trackCount: Int,
        val format: String,
        val country: String,
        val label: String,
        /** It lands in "<artist> Singles". */
        val isSingle: Boolean,
    )

    private var handle = nativeStart(
        library.handle,
        paths.toTypedArray(),
        options.fetchArt,
        options.skipTagging,
        options.stagingDir,
        this,
    )

    /** How many items the scan found; 0 until [Event.Scanned]. */
    val itemCount: Int get() = nativeItemCount(handle)

    fun itemState(item: Int): State = State.entries[nativeItemState(handle, item)]

    /** "Artist – Album" from the item's tags, else its folder, in the core's language. */
    fun itemName(item: Int): String = nativeItemName(handle, item)

    /**
     * Why the item failed or needs review, e.g. "Couldn't reach MusicBrainz", or null: in the
     * language given to [NativeCore.setLocale] when it happened.
     */
    fun itemError(item: Int): String? = nativeItemError(handle, item)

    fun itemInfo(item: Int): ItemInfo {
        val f = nativeItemInfo(handle, item)
        return ItemInfo(
            kind = Kind.entries[f[0].toInt()],
            state = State.entries[f[1].toInt()],
            fileCount = f[2].toInt(),
            artist = f[3],
            album = f[4],
            folder = f[5],
            error = f[6].ifEmpty { null },
            searchedWider = f[7] != "0",
            outcome = Outcome.entries.getOrElse(f[8].toInt()) { Outcome.None },
            chosen = f[9].toInt(),
            matchCount = f[10].toInt(),
        )
    }

    /** The item's candidates, best first. */
    fun matches(item: Int): List<Match> {
        val f = nativeItemMatches(handle, item)
        return (0 until f.size / MATCH_FIELDS).map { i ->
            val at = i * MATCH_FIELDS
            Match(
                album = f[at],
                albumArtist = f[at + 1],
                release = f[at + 2],
                year = f[at + 3].toInt(),
                confidence = f[at + 4].toInt() / 1000.0,
                trackCount = f[at + 5].toInt(),
                format = f[at + 6],
                country = f[at + 7],
                label = f[at + 8],
                isSingle = f[at + 9] != "0",
            )
        }
    }

    /** Imports an item that needs review with candidate [match]. */
    fun accept(item: Int, match: Int) = nativeAccept(handle, item, match)

    /** Imports an item that needs review with the tags it already has. */
    fun acceptAsIs(item: Int) = nativeResolve(handle, item, 0)
    fun skip(item: Int) = nativeResolve(handle, item, 1)

    /** Searches MusicBrainz more widely for an album; the item stays in review. */
    fun searchWider(item: Int) = nativeResolve(handle, item, 2)

    /** Searches again with the user's own terms; [title] is the album, or the track for a single. */
    fun search(item: Int, artist: String?, title: String?) = nativeSearch(handle, item, artist, title)

    /** Imports everything not matched yet with its existing tags, for an offline import. */
    fun skipTagging() = nativeSkipTagging(handle)

    /** Whether MusicBrainz was unreachable at the last check (see [Event.Offline]). */
    val offline: Boolean get() = nativeOffline(handle)

    /** Stops the session and waits for the import thread. What's already written stays. */
    override fun close() {
        if (handle != 0L) nativeFree(handle)
        handle = 0
    }

    // Called from jni_bridge.c.
    @Suppress("unused")
    private fun onEvent(event: Int, arg: Int) {
        Event.entries.getOrNull(event)?.let { listener(it, arg) }
    }

    private companion object {
        const val MATCH_FIELDS = 10 // jni_bridge.c

        init {
            System.loadLibrary("kithsong_jni")
        }

        @JvmStatic external fun nativeStart(
            library: Long,
            paths: Array<String>,
            fetchArt: Boolean,
            skipTagging: Boolean,
            stagingDir: String?,
            listener: CoreImport,
        ): Long
        @JvmStatic external fun nativeFree(handle: Long)
        @JvmStatic external fun nativeItemCount(handle: Long): Int
        @JvmStatic external fun nativeItemState(handle: Long, item: Int): Int
        @JvmStatic external fun nativeItemName(handle: Long, item: Int): String
        @JvmStatic external fun nativeItemError(handle: Long, item: Int): String?
        @JvmStatic external fun nativeItemInfo(handle: Long, item: Int): Array<String>
        @JvmStatic external fun nativeItemMatches(handle: Long, item: Int): Array<String>
        @JvmStatic external fun nativeResolve(handle: Long, item: Int, how: Int)
        @JvmStatic external fun nativeAccept(handle: Long, item: Int, match: Int)
        @JvmStatic external fun nativeSearch(handle: Long, item: Int, artist: String?, title: String?)
        @JvmStatic external fun nativeSkipTagging(handle: Long)
        @JvmStatic external fun nativeOffline(handle: Long): Boolean
    }
}
