package ng.kithso.core

// Making and editing smart playlists and smart album groups (musiccore/smartlist.h), bound in
// jni_smart.c. A rule is JSON in the format docs/schema.md gives (Smart lists); [isAlbums]
// says which subject it's for, an album group's rather than a playlist's. Listing them is
// CoreLibrary.smartLists, and what a rule would hold previewSmartTracks / previewSmartAlbums.
// Each throws KithCoreException: MC_ERR_FORMAT (-7) for a rule that isn't valid,
// MC_ERR_NOT_FOUND (-8) for a smart list that doesn't exist.

/**
 * mc_smart_field: a field a rule can test, or if [sortable] order by. [type] is "text", "int",
 * "duration" (seconds), "date" (Unix ms, or {"days": n}), "bool", "playlist", "group" or
 * "format"; [ops] are the operators it takes, in the core's order.
 */
data class SmartField(val name: String, val type: String, val ops: List<String>, val sortable: Boolean) {
    internal constructor(name: String, type: String, ops: Array<String>, sortable: Boolean) :
        this(name, type, ops.asList(), sortable)
}

/**
 * What the core knows of rules without a library: their fields and the words for an editor,
 * in the language given to [NativeCore.setLocale], so every platform's editor reads alike.
 */
object CoreSmart {
    /** The fields a smart playlist's rule, or if [isAlbums] a smart album group's, can use. */
    fun fields(isAlbums: Boolean): List<SmartField> = SmartNatives.nativeFields(isAlbums).asList()

    /** A field's label for a picker, e.g. "Last played"; also "Random" for "random". */
    fun fieldLabel(field: String): String? = SmartNatives.nativeFieldLabel(field)

    /** An operator's label after a field of [type], e.g. "is less than" for ("int", "<"). */
    fun opLabel(type: String, op: String): String? = SmartNatives.nativeOpLabel(type, op)

    /** A sort order's label, e.g. "least played first"; null for a field nothing sorts on. */
    fun orderLabel(field: String, descending: Boolean): String? = SmartNatives.nativeOrderLabel(field, descending)

    /** Whether [rule] is valid, without saving anything (mc_smart_validate). */
    fun isValid(isAlbums: Boolean, rule: String): Boolean = SmartNatives.nativeValidate(isAlbums, rule)
}

/**
 * [rule] as a sentence for showing under a list's name or in the editor (mc_smart_describe),
 * e.g. "Tracks where genre contains “jazz” · least played first · 10 tracks".
 */
fun CoreLibrary.describeSmart(isAlbums: Boolean, rule: String): String = SmartNatives.nativeDescribe(handle, isAlbums, rule)

/** How many tracks or albums [rule] picks, at most its limit, counted without listing them (mc_smart_count). */
fun CoreLibrary.countSmart(isAlbums: Boolean, rule: String): Int = SmartNatives.nativeCount(handle, isAlbums, rule)

/** A new smart list called [name] (not blank) with [rule]; returns its uid. */
fun CoreLibrary.createSmart(name: String, isAlbums: Boolean, rule: String): String =
    SmartNatives.nativeCreate(handle, name, isAlbums, rule)

fun CoreLibrary.renameSmart(smartUid: String, name: String) = SmartNatives.nativeRename(handle, smartUid, name)

fun CoreLibrary.setSmartRule(smartUid: String, rule: String) = SmartNatives.nativeSetRule(handle, smartUid, rule)

/** Deletes the smart list; what it picked stays in the library. */
fun CoreLibrary.deleteSmart(smartUid: String) = SmartNatives.nativeDelete(handle, smartUid)

internal object SmartNatives {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun nativeFields(albums: Boolean): Array<SmartField>
    @JvmStatic external fun nativeFieldLabel(field: String): String?
    @JvmStatic external fun nativeOpLabel(type: String, op: String): String?
    @JvmStatic external fun nativeOrderLabel(field: String, desc: Boolean): String?
    @JvmStatic external fun nativeValidate(albums: Boolean, rule: String): Boolean
    @JvmStatic external fun nativeDescribe(handle: Long, albums: Boolean, rule: String): String
    @JvmStatic external fun nativeCount(handle: Long, albums: Boolean, rule: String): Int
    @JvmStatic external fun nativeCreate(handle: Long, name: String, albums: Boolean, rule: String): String
    @JvmStatic external fun nativeRename(handle: Long, uid: String, name: String)
    @JvmStatic external fun nativeSetRule(handle: Long, uid: String, rule: String)
    @JvmStatic external fun nativeDelete(handle: Long, uid: String)
}
