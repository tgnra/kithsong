package ng.kithso.core

// Removing an album or artist from this phone only, and syncing it here again
// (kithcore/sync_here.h), bound in jni_sync_here.c. It stays in the library and on the other
// devices: this phone's profile gets an exclusion, and its files here go at once. Each throws
// KithCoreException.

/** mc_here_state: what an album's or artist's menu offers. */
enum class HereState {
    /** None: sync is off, not in a group, or this phone is an archive. */
    Unavailable,

    /** Remove from this phone. */
    Held,

    /** Removed from this phone, or an album with its artist: Sync to this phone. */
    Removed,

    /** This phone's selection doesn't have it: Add to this phone. */
    NotIncluded,
}

/**
 * What removing would do: [files] go from this phone now, [kept] imported originals stay until
 * an archive has them, and [staying] tracks stay because a ticked playlist has them.
 */
data class HerePreview(val files: Int, val kept: Int, val staying: Int)

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
    SyncHereNatives.nativePreview(handle, kind.code(), uid).let { HerePreview(it[0], it[1], it[2]) }

/** Whether removing [uid] from this phone would remove a file of any of [trackUids], e.g. the play queue's. */
fun CoreSync.hereTakesAny(kind: ProfileItemKind, uid: String, trackUids: List<String>): Boolean =
    trackUids.isNotEmpty() && SyncHereNatives.nativeTakesAny(handle, kind.code(), uid, trackUids.toTypedArray())

/** Removes [uid] from this phone: its profile, then its files here. */
fun CoreSync.removeHere(kind: ProfileItemKind, uid: String): HereResult =
    SyncHereNatives.nativeRemove(handle, kind.code(), uid).let { HereResult(it[0], it[1]) }

/** Undoes [removeHere], or adds [uid] to this phone's selection; the files come at the next sync. */
fun CoreSync.restoreHere(kind: ProfileItemKind, uid: String) =
    SyncHereNatives.nativeRestore(handle, kind.code(), uid)

/**
 * A tick under Sync everything on [deviceUid]'s profile: with [on], excludes [uid], else takes
 * it out of what's removed, an album coming back alone if its artist stays removed.
 */
fun CoreSync.excludeItem(deviceUid: String, kind: ProfileItemKind, uid: String, on: Boolean) =
    SyncHereNatives.nativeExclude(handle, deviceUid, kind.code(), uid, on)

/**
 * Select all ([on]) or Select none under What to sync: [uids] of [kind], in one write. Select all
 * leaves out what's removed from the device (mc_sync_select_items). Store pending edits first.
 */
fun CoreSync.selectItems(deviceUid: String, kind: ProfileItemKind, uids: List<String>, on: Boolean) =
    SyncHereNatives.nativeSelect(handle, deviceUid, kind.code(), uids.toTypedArray(), on)

/**
 * Whether deletes here go to the recycle bin ([CoreLibrary.deleteToBin]): in a group, and not
 * an archive, as for [removeHere].
 */
val CoreSync.binOffered: Boolean get() = SyncHereNatives.nativeBinOffered(handle)

/**
 * How many deletes the recycle bin holds. While any, sync starts nothing by itself and turns
 * other devices away: only [CoreSync.confirmSyncNow] or [CoreSync.syncNow] send them, for good.
 */
val CoreSync.binCount: Int get() = SyncHereNatives.nativeBinCount(handle)

/**
 * Takes [id] back out of the recycle bin: it's back in the library, removed from this phone
 * (its files stay gone), and no other device hears of the delete. Returns how many entries
 * came back (one, or more where a later delete took what it needs). ERR_NOT_FOUND once a sync
 * has sent it, ERR_LOCKED while one is sending.
 */
fun CoreSync.binRestore(id: Long): Int = SyncHereNatives.nativeBinRestore(handle, id)

internal object SyncHereNatives {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun nativeState(handle: Long, kind: Int, uid: String): Int
    @JvmStatic external fun nativePreview(handle: Long, kind: Int, uid: String): IntArray
    @JvmStatic external fun nativeTakesAny(handle: Long, kind: Int, uid: String, trackUids: Array<String>): Boolean
    @JvmStatic external fun nativeRemove(handle: Long, kind: Int, uid: String): IntArray
    @JvmStatic external fun nativeRestore(handle: Long, kind: Int, uid: String)
    @JvmStatic external fun nativeExclude(handle: Long, deviceUid: String, kind: Int, uid: String, on: Boolean)
    @JvmStatic external fun nativeSelect(handle: Long, deviceUid: String, kind: Int, uids: Array<String>, on: Boolean)
    @JvmStatic external fun nativeBinOffered(handle: Long): Boolean
    @JvmStatic external fun nativeBinCount(handle: Long): Int
    @JvmStatic external fun nativeBinRestore(handle: Long, id: Long): Int
}
