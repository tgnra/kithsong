package ng.kithso.core

// Deleting albums and tracks from an open CoreLibrary (kithcore/delete.h), bound in
// jni_delete.c. Android has no trash, so a delete removes the files for good
// (mc_delete_run_permanently); like any change it's logged, so sync deletes the tracks on every
// device. In a group ([binOffered]) it goes to the recycle bin as well (mc_delete_run_to_bin),
// from which it can be taken back until the next sync ([binRestore]). Each throws
// KithCoreException.

/** What a delete takes: a track, or an album's tracks (mc_delete_kind). */
enum class DeleteKind(internal val code: Int) {
    Track(0),
    Album(1),
}

/** What a delete would take: [tracks] from the library, [files] of them (and covers) from this phone. */
data class DeletePreview(val tracks: Int, val files: Int)

/**
 * What a delete did: [tracksDeleted] left the library, [filesRemoved] went from this phone, and
 * [tracksKept] stayed, as one of their files couldn't be removed.
 */
data class DeleteResult(val tracksDeleted: Int, val filesRemoved: Int, val tracksKept: Int)

/** What deleting [uid] of [kind] would delete, to ask first; nothing if it's gone already. */
fun CoreLibrary.deletePreview(kind: DeleteKind, uid: String): DeletePreview =
    DeleteNatives.nativePreview(handle, kind.code, uid).let { DeletePreview(it[0], it[1]) }

/** Whether deleting [uid] of [kind] would delete any of [trackUids], e.g. the play queue's. */
fun CoreLibrary.deleteTakesAny(kind: DeleteKind, uid: String, trackUids: List<String>): Boolean =
    trackUids.isNotEmpty() && DeleteNatives.nativeTakesAny(handle, kind.code, uid, trackUids.toTypedArray())

/** Deletes [uid] of [kind] from the library, and its files from this phone for good. */
fun CoreLibrary.deletePermanently(kind: DeleteKind, uid: String): DeleteResult =
    DeleteNatives.nativeDelete(handle, kind.code, uid).let { DeleteResult(it[0], it[1], it[2]) }

/** As [deletePermanently], keeping the delete in the recycle bin until the next sync. */
fun CoreLibrary.deleteToBin(kind: DeleteKind, uid: String): DeleteResult =
    DeleteNatives.nativeDeleteToBin(handle, kind.code, uid).let { DeleteResult(it[0], it[1], it[2]) }

/**
 * A delete in the recycle bin (mc_bin_entry): [items] albums or tracks by [kind], the first
 * called [title] by [artist], [tracks] in all, deleted at [deletedMs] (Unix ms).
 */
data class BinEntry(
    val id: Long,
    private val kindCode: Int,
    val items: Int,
    val title: String?,
    val artist: String?,
    val tracks: Int,
    val deletedMs: Long,
) {
    val kind: DeleteKind get() = DeleteKind.entries.firstOrNull { it.code == kindCode } ?: DeleteKind.Track
}

/** The recycle bin's entries, newest first. */
fun CoreLibrary.binEntries(): List<BinEntry> = DeleteNatives.nativeBinList(handle).toList()

internal object DeleteNatives {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun nativePreview(handle: Long, kind: Int, uid: String): IntArray
    @JvmStatic external fun nativeDelete(handle: Long, kind: Int, uid: String): IntArray
    @JvmStatic external fun nativeTakesAny(handle: Long, kind: Int, uid: String, trackUids: Array<String>): Boolean
    @JvmStatic external fun nativeDeleteToBin(handle: Long, kind: Int, uid: String): IntArray
    @JvmStatic external fun nativeBinList(handle: Long): Array<BinEntry>
}
