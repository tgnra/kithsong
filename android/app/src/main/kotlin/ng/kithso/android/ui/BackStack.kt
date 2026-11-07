package ng.kithso.android.ui

import androidx.compose.runtime.Immutable
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.listSaver
import ng.kithso.android.data.CollectionRef
import java.net.URLDecoder
import java.net.URLEncoder

/**
 * One page of a [BackStack]; [id] tells two openings of the same page apart. A [shifted] page moves
 * home's columns over by one while it's open: see [BackStack.besideHome].
 */
@Immutable
data class BackLayer<T>(val id: Int, val page: T, val leaving: Boolean = false, val shifted: Boolean = false)

/**
 * Pages stacked over a screen, bottom first, as back goes through them: back goes to [top],
 * and from there down one page at a time. A page that's been dismissed stays in [layers] as
 * [BackLayer.leaving] while it slides out, so it's still drawn, but it's already off the stack
 * as far as back is concerned: the next press is for the page beneath, however soon it comes.
 *
 * A value, so a change is one assignment to the state holding it.
 */
@Immutable
data class BackStack<T>(val layers: List<BackLayer<T>> = emptyList(), private val nextId: Int = 0) {
    /** The page back goes to next; null when there's none, and back is the screen beneath's. */
    val top: BackLayer<T>? get() = layers.lastOrNull { !it.leaving }

    /** The pages still open, bottom first. */
    val pages: List<T> get() = layers.filterNot { it.leaving }.map { it.page }

    /** [page] over the others, unless it's on top already: a double tap opens it once. */
    fun push(page: T): BackStack<T> {
        if (top?.page == page) return this
        onPush(page)
        return BackStack(layers + BackLayer(nextId, page), nextId + 1)
    }

    /** Whether a page open now has home's columns moved over. */
    val shifted: Boolean get() = layers.any { it.shifted && !it.leaving }

    /**
     * [page], opened from home beside the pages: it takes the place of all of them, so back goes
     * straight home from it. They stay drawn beneath it until [settle], so it arrives over them,
     * not over a gap. A page opened from a page still goes through [push], and stacks.
     */
    fun besideHome(page: T, shifted: Boolean = false): BackStack<T> {
        val current = top
        if (current?.page == page) return copy(layers = layers.filter { it.leaving || it.id == current.id })
        onPush(page)
        return BackStack(layers.map { it.copy(leaving = true) } + BackLayer(nextId, page, shifted = shifted), nextId + 1)
    }

    /** Without the layers on their way out beneath [id], now it covers them or has gone. */
    fun settle(id: Int): BackStack<T> {
        val at = layers.indexOfFirst { it.id == id }
        return copy(layers = layers.filterIndexed { i, layer -> i >= at || !layer.leaving })
    }

    /** Off the stack for back, and still drawn until [remove]. */
    fun leave(id: Int): BackStack<T> = copy(layers = layers.map { if (it.id == id) it.copy(leaving = true) else it })

    /** Gone, slid out or not. */
    fun remove(id: Int): BackStack<T> = copy(layers = layers.filterNot { it.id == id })

    /** Without the layers [gone] says, at once. */
    fun removeAll(gone: (T) -> Boolean): BackStack<T> = copy(layers = layers.filterNot { gone(it.page) })

    /** Only the lowest [count] open pages; the rest go at once. */
    fun keep(count: Int): BackStack<T> = copy(layers = layers.filterNot { it.leaving }.take(count))

    companion object {
        private const val SHIFTED = "s"

        /** Told of each page pushed. The app sets it, so this class and its tests need no core. */
        var onPush: (Any?) -> Unit = {}

        fun <T> of(pages: List<T>): BackStack<T> = pages.fold(BackStack()) { stack, page -> stack.push(page) }

        /** Keeps the open pages, each with its id, so what they've saved finds them again. */
        fun <T> saver(encode: (T) -> String, decode: (String) -> T): Saver<BackStack<T>, *> = listSaver(
            save = { stack -> stack.layers.filterNot { it.leaving }.map { "${it.id}${if (it.shifted) SHIFTED else ""}:${encode(it.page)}" } },
            restore = { saved ->
                val layers = saved.map {
                    val head = it.substringBefore(':')
                    BackLayer(head.removeSuffix(SHIFTED).toInt(), decode(it.substringAfter(':')), shifted = head.endsWith(SHIFTED))
                }
                BackStack(layers, (layers.maxOfOrNull { it.id } ?: -1) + 1)
            },
        )
    }
}

/** What opens over the main list. The player is always the top page while it's open. */
sealed interface Page {
    data object Settings : Page
    data object Licenses : Page

    /** A licence's text, over the licences: its asset's path. */
    data class License(val asset: String) : Page
    data object Sync : Page

    /** Over Settings or Sync, either of which opens it. */
    data object RecycleBin : Page

    /** A device's page, over Sync: a member's uid, or THIS_DEVICE. */
    data class Device(val uid: String) : Page
    data object Search : Page
    data object Stats : Page

    /** A track list, over the main list or the search it was found in. */
    data class Collection(val ref: CollectionRef) : Page
    data object Player : Page

    companion object {
        fun encode(page: Page): String = when (page) {
            Settings -> "settings"
            Licenses -> "licenses"
            is License -> "license:${page.asset}"
            Sync -> "sync"
            RecycleBin -> "bin"
            is Device -> "device:${page.uid}"
            Search -> "search"
            Stats -> "stats"
            is Collection -> page.ref.group?.let { "grouplist:${URLEncoder.encode(it, "UTF-8")}:${page.ref.uid}" }
                ?: "list:${page.ref.kind.name}:${page.ref.uid}"
            Player -> "player"
        }

        fun decode(saved: String): Page {
            val rest = saved.substringAfter(':')
            return when (saved.substringBefore(':')) {
                "settings" -> Settings
                "licenses" -> Licenses
                "license" -> License(rest)
                "sync" -> Sync
                "bin" -> RecycleBin
                "device" -> Device(rest)
                "search" -> Search
                "stats" -> Stats
                "list" -> Collection(
                    CollectionRef(CollectionRef.Kind.valueOf(rest.substringBefore(':')), rest.substringAfter(':')),
                )
                "grouplist" -> Collection(
                    CollectionRef(
                        CollectionRef.Kind.Album,
                        rest.substringAfter(':'),
                        URLDecoder.decode(rest.substringBefore(':'), "UTF-8"),
                    ),
                )
                else -> Player
            }
        }
    }
}
