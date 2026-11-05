package ng.kithso.android.data

import ng.kithso.core.HerePreview
import ng.kithso.core.HereResult
import ng.kithso.core.HereState
import ng.kithso.core.ProfileItemKind

/**
 * Remove from this phone and Sync to this phone, for albums and artists
 * (musiccore/sync_here.h): an album or artist leaves this phone, or comes back, while it stays
 * in the library and on the other devices. This phone's sync profile gets an exclusion and
 * its files here go at once. Neither is offered outside a group or on an archive
 * ([HereState.Unavailable]), where Delete from library is the only honest option. The sync
 * side ([ng.kithso.android.sync.Syncer]) does it; [kind] is [ProfileItemKind.Album] or
 * [ProfileItemKind.Artist].
 *
 * As with a delete ([LibraryDeleter]), a file that's playing could still be removed here, but
 * the track would play on to its end and the queue's other removed tracks be skipped, so as
 * on Windows the UI stops playback first, emptying the queue, if the removal takes any of its
 * tracks ([takesAny]).
 */
interface DeviceRemover {
    /** What [uid]'s menu offers; [HereState.Unavailable] if sync isn't open. */
    suspend fun hereState(kind: ProfileItemKind, uid: String): HereState

    /** What removing [uid] would do, to ask first; null if that couldn't be found out. */
    suspend fun herePreview(kind: ProfileItemKind, uid: String): HerePreview?

    /** Whether removing [uid] would remove a file of any of [trackUids]; false if that couldn't be found out. */
    suspend fun takesAny(kind: ProfileItemKind, uid: String, trackUids: List<String>): Boolean

    /** Removes [uid] from this phone, then reads the library again; null if it failed. */
    suspend fun removeHere(kind: ProfileItemKind, uid: String): HereResult?

    /** Syncs [uid] to this phone again, or adds it to a selection, starting a sync; false if it failed. */
    suspend fun restoreHere(kind: ProfileItemKind, uid: String): Boolean
}
