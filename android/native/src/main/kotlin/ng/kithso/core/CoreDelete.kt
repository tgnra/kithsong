package ng.kithso.core

// Deleting albums, artists and tracks from an open CoreLibrary (musiccore/delete.h), bound in
// jni_delete.c. Android has no trash, so a delete removes the files for good
// (mc_delete_run_permanently) and can't be restored; like any change it's logged, so sync
// deletes the tracks on every device. Each throws MusicCoreException.

/** What a delete takes: a track, an album's tracks, or an artist's (mc_delete_kind). */
enum class DeleteKind(internal val code: Int) {
    Track(0),
    Album(1),
    /** Every track credited to the artist, and every track of their albums. */
    Artist(2),
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

internal object DeleteNatives {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun nativePreview(handle: Long, kind: Int, uid: String): IntArray
    @JvmStatic external fun nativeDelete(handle: Long, kind: Int, uid: String): IntArray
    @JvmStatic external fun nativeTakesAny(handle: Long, kind: Int, uid: String, trackUids: Array<String>): Boolean
}
