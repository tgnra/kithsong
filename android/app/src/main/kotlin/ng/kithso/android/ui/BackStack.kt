package ng.kithso.android.ui

import androidx.compose.runtime.Immutable
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.listSaver
import ng.kithso.android.data.CollectionRef

/** One page of a [BackStack]; [id] tells two openings of the same page apart. */
@Immutable
data class BackLayer<T>(val id: Int, val page: T, val leaving: Boolean = false)

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
        // The click: App ends the view switch once the page is drawn.
        // By toString(), as R8 renames classes but not what a data class says it's called.
        if (ng.kithso.core.CoreTesting.enabled()) ng.kithso.core.CoreTesting.viewBegin(page.toString().substringBefore('('))
        return BackStack(layers + BackLayer(nextId, page), nextId + 1)
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
        fun <T> of(pages: List<T>): BackStack<T> = pages.fold(BackStack()) { stack, page -> stack.push(page) }

        /** Keeps the open pages, each with its id, so what they've saved finds them again. */
        fun <T> saver(encode: (T) -> String, decode: (String) -> T): Saver<BackStack<T>, *> = listSaver(
            save = { stack -> stack.layers.filterNot { it.leaving }.map { "${it.id}:${encode(it.page)}" } },
            restore = { saved ->
                val layers = saved.map { BackLayer(it.substringBefore(':').toInt(), decode(it.substringAfter(':'))) }
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
            is Collection -> "list:${page.ref.kind.name}:${page.ref.uid}"
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
                "list" -> Collection(
                    CollectionRef(CollectionRef.Kind.valueOf(rest.substringBefore(':')), rest.substringAfter(':')),
                )
                else -> Player
            }
        }
    }
}
