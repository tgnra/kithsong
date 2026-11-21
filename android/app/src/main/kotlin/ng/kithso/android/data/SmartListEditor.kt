package ng.kithso.android.data

import androidx.compose.runtime.Immutable

/** A smart list as saved: a smart playlist, or a smart album group if [isAlbums]. [rule] is its JSON. */
@Immutable
data class SmartListInfo(val uid: String, val name: String, val isAlbums: Boolean, val rule: String)

/** A track or album a rule picks, as the editor's preview lists it. */
@Immutable
data class SmartPreviewItem(val title: String, val artist: String)

/** What a rule picks now: the first [items] of [count], or what it alone matches for a group's page. */
@Immutable
data class SmartPreview(val items: List<SmartPreviewItem>, val count: Int)

/**
 * Making, editing, renaming and deleting smart playlists and smart album groups, for a
 * [LibrarySource] that can ([CoreLibrarySource]). Changes go through on the library's own
 * thread and the library is read again, as [PlaylistEditor]'s do, counted in its
 * [PlaylistEditor.edits]; a failure is logged and reported in its [PlaylistEditor.failures].
 * The counts and previews the editor shows as a rule is edited are worked out on a thread and
 * library connection of their own, so they don't hold up the rest of the app. A rule is JSON
 * (docs/schema.md, Smart lists), checked by the core before it's saved.
 */
interface SmartListEditor {
    /** Whether [uid] is a smart playlist or smart album group. */
    suspend fun isSmart(uid: String): Boolean

    /** Smart list [uid] as saved, or null if it's gone. */
    suspend fun smartList(uid: String): SmartListInfo?

    /** What a playlist and an album group condition can pick: every manual one, by name; null on failure. */
    suspend fun smartChoices(): Pair<List<PlaylistChoice>, List<PlaylistChoice>>?

    /** [rule] as a sentence (mc_smart_describe), or null if it isn't valid. */
    suspend fun describeRule(isAlbums: Boolean, rule: String): String?

    /** How many tracks or albums [rule] picks, or null if that couldn't be worked out. */
    suspend fun countRule(isAlbums: Boolean, rule: String): Int?

    /** What [rule] picks: the first [max] of them, and how many in all; null if it couldn't be worked out. */
    suspend fun previewRule(isAlbums: Boolean, rule: String, max: Int): SmartPreview?

    /** Makes a smart list called [name] with [rule]; its uid, or null if that failed. */
    suspend fun createSmart(name: String, isAlbums: Boolean, rule: String): String?

    /** Renames smart list [uid] to [name] and sets its [rule], each only if not null. False if either failed. */
    suspend fun saveSmart(uid: String, name: String?, rule: String?): Boolean

    fun renameSmart(uid: String, name: String)

    /** Deletes the smart list; what it picks stays in the library. */
    fun deleteSmart(uid: String)
}
