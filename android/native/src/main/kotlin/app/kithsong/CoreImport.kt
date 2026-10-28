package app.kithsong

/**
 * An import session (mc_import): scans [paths] on a core-owned thread, matches what it finds
 * against MusicBrainz and copies it into [library], which must stay open until this is
 * closed. [listener] is called on the import thread with an [Event] and its argument (see
 * mc_import_event), possibly before the constructor has returned, so it should hand events
 * to another thread rather than call back into this. Everything else may be called from any
 * thread.
 */
class CoreImport(
    library: CoreLibrary,
    paths: List<String>,
    fetchArt: Boolean,
    private val listener: (Event, Int) -> Unit,
) : AutoCloseable {
    enum class Event { Scanning, Scanned, ItemChanged, Idle }

    enum class State {
        Pending, Matching, Review, Writing, Done, Skipped, Failed;

        val isFinished: Boolean get() = this == Done || this == Skipped || this == Failed
    }

    private var handle = nativeStart(library.handle, paths.toTypedArray(), fetchArt, this)

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

    /** Imports an item that needs review with the tags it already has. */
    fun acceptAsIs(item: Int) = nativeResolve(handle, item, 0)
    fun skip(item: Int) = nativeResolve(handle, item, 1)

    /** Stops the session and waits for the import thread. What's already written stays. */
    override fun close() {
        if (handle != 0L) nativeFree(handle)
        handle = 0
    }

    // Called from jni_bridge.c.
    @Suppress("unused")
    private fun onEvent(event: Int, arg: Int) = listener(Event.entries[event], arg)

    private companion object {
        init {
            System.loadLibrary("kithsong_jni")
        }

        @JvmStatic external fun nativeStart(
            library: Long,
            paths: Array<String>,
            fetchArt: Boolean,
            listener: CoreImport,
        ): Long
        @JvmStatic external fun nativeFree(handle: Long)
        @JvmStatic external fun nativeItemCount(handle: Long): Int
        @JvmStatic external fun nativeItemState(handle: Long, item: Int): Int
        @JvmStatic external fun nativeItemName(handle: Long, item: Int): String
        @JvmStatic external fun nativeItemError(handle: Long, item: Int): String?
        @JvmStatic external fun nativeResolve(handle: Long, item: Int, how: Int)
    }
}
