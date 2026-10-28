package ng.kithso.core

// Removing an album or artist from this phone only, and syncing it here again
// (musiccore/sync_here.h), bound in jni_sync_here.c. It stays in the library and on the other
// devices: this phone's profile gets an exclusion, and its files here go at once. Each throws
// MusicCoreException.

/** mc_here_state: what an album's or artist's menu offers. */
enum class HereState {
    /** Neither: sync is off, not in a group, or this phone is an archive. */
    Unavailable,

    /** Remove from this phone. */
    Held,

    /** Removed from this phone: Sync to this phone. */
    Removed,
}

/** What removing would do: [files] go from this phone now, [kept] imported originals stay until an archive has them. */
data class HerePreview(val files: Int, val kept: Int)

/** What removing did: [filesRemoved] went, [filesKept] stayed (imported here, or couldn't be removed). */
data class HereResult(val filesRemoved: Int, val filesKept: Int)

private fun ProfileItemKind.code(): Int {
    require(this == ProfileItemKind.Album || this == ProfileItemKind.Artist) { "albums and artists only" }
    return ordinal
}

/** What [uid], an album or artist by [kind], offers on its menu. */
fun CoreSync.hereState(kind: ProfileItemKind, uid: String): HereState =
    HereState.entries[SyncHereNatives.nativeState(handle, kind.code(), uid)]

/** What removing [uid] from this phone would do, to ask first. */
fun CoreSync.herePreview(kind: ProfileItemKind, uid: String): HerePreview =
    SyncHereNatives.nativePreview(handle, kind.code(), uid).let { HerePreview(it[0], it[1]) }

/** Removes [uid] from this phone: its profile, then its files here. */
fun CoreSync.removeHere(kind: ProfileItemKind, uid: String): HereResult =
    SyncHereNatives.nativeRemove(handle, kind.code(), uid).let { HereResult(it[0], it[1]) }

/** Undoes [removeHere]; the files come back at the next sync. */
fun CoreSync.restoreHere(kind: ProfileItemKind, uid: String) =
    SyncHereNatives.nativeRestore(handle, kind.code(), uid)

internal object SyncHereNatives {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun nativeState(handle: Long, kind: Int, uid: String): Int
    @JvmStatic external fun nativePreview(handle: Long, kind: Int, uid: String): IntArray
    @JvmStatic external fun nativeRemove(handle: Long, kind: Int, uid: String): IntArray
    @JvmStatic external fun nativeRestore(handle: Long, kind: Int, uid: String)
}
