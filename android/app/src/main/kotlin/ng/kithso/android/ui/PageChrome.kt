package ng.kithso.android.ui

import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.statusBars
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.staticCompositionLocalOf

/** What a page's button at the top left does, where [PageChrome] says. */
enum class PageBack {
    /** Back, as on the phone, where each page covers the one it opened from. */
    Back,

    /** Closes the page, in the tablet layout's pane beside the content. */
    Close,

    /** Nothing to go back to: a page the tablet layout's sidebar picked. */
    None,
}

/**
 * Where a page is shown. On the phone every page has the whole window, under the status bar and
 * between whatever's at the sides; the tablet layout puts pages beneath its top bar and beside
 * its sidebar, which keep the system bars clear themselves. A page asks this, through
 * [pageTop] and [pageSides], rather than the window's insets, which a [WindowInsets] read as
 * padding values can't be told have been taken care of. [back] is what its back button does,
 * and [inTablet] says it's in the tablet layout, where an album grid keeps clear of a fold.
 */
@Immutable
data class PageChrome(
    val top: WindowInsets? = null,
    val sides: WindowInsets? = null,
    val back: PageBack = PageBack.Back,
    val inTablet: Boolean = false,
)

/** The phone's: the window's insets, and a back button. */
val LocalPageChrome = staticCompositionLocalOf { PageChrome() }

/** What a page's header keeps clear of at the top: the status bar, on the phone. */
@Composable
fun pageTop(): WindowInsets = LocalPageChrome.current.top ?: WindowInsets.statusBars

/** What a page's content keeps clear of at the sides: a camera cutout or navigation bar, on the phone. */
@Composable
fun pageSides(): WindowInsets = LocalPageChrome.current.sides ?: WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
