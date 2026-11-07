package ng.kithso.core

// The Home page's rows (kithcore/home_picks.h), bound in jni_home.c.

/** mc_home_row_id, in its order: the Home page's rows, top to bottom. */
enum class HomeRowId { Radio, Recents, PlayedLately, NotPlayedLately, Random }

/**
 * mc_home_pick: a tile of [row]. [kind] is what it opens, and [uid] which; a smart playlist or
 * album group [isSmart]. A station ([isStation]) is an [ItemKind.Artist] whose [uid] is its seed's
 * artist's, empty if that's unknown, so it's [seedUid], its seed track, that says which station it
 * is; one of the Radio row has [startUid], where it starts this time. [sub] is the line beneath
 * the title, in the core's language. [coverDir] holds its cover: null for none, and for playlists
 * and album groups, whose pictures are found by uid. [seedTitle] is a station's seed's title.
 */
data class HomePick(
    val row: HomeRowId,
    val kind: ItemKind,
    val isStation: Boolean,
    val isSmart: Boolean,
    val uid: String,
    val seedUid: String?,
    val startUid: String?,
    val title: String,
    val sub: String?,
    val coverDir: String?,
    val seedTitle: String?,
) {
    internal constructor(
        row: Int, kind: Int, isStation: Boolean, isSmart: Boolean, uid: String, seedUid: String?, startUid: String?,
        title: String, sub: String?, coverDir: String?, seedTitle: String?,
    ) : this(
        HomeRowId.entries[row], ItemKind.entries[kind], isStation, isSmart, uid, seedUid, startUid, title, sub, coverDir, seedTitle,
    )
}

/**
 * mc_home_picks: each row's tiles, in order; a row with none isn't in [rows]. [hasMusic] is a
 * library with an album or playlist to show, and [hasPlays] one with something in it played.
 */
data class HomePicks(val rows: Map<HomeRowId, List<HomePick>>, val hasMusic: Boolean, val hasPlays: Boolean) {
    internal constructor(picks: Array<HomePick>, hasMusic: Boolean, hasPlays: Boolean) :
        this(picks.groupBy { it.row }, hasMusic, hasPlays)
}

/**
 * The Home page's rows as of now (mc_home_picks_load). [seed] decides Random's selection and
 * order, so one seed keeps the same selection across reloads. It lists every album, evaluates
 * every smart list, and reads every analysis result and the last 90 days of the play log, so
 * it's for a background thread, on a library connection of its own.
 */
fun CoreLibrary.homePicks(seed: Int): HomePicks = HomeNatives.nativeLoad(handle, seed)

internal object HomeNatives {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun nativeLoad(handle: Long, seed: Int): HomePicks
}
