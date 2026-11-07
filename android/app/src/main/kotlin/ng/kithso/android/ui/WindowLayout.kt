package ng.kithso.android.ui

import android.content.pm.PackageManager
import android.util.Log
import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.absoluteOffset
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.wrapContentWidth
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.AbsoluteAlignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.window.layout.FoldingFeature
import androidx.window.layout.WindowInfoTracker
import kotlin.math.roundToInt

/** Which of the app's layouts a window gets. See [windowLayoutClass]. */
enum class WindowLayoutClass {
    /** Today's phone UI. */
    Compact,

    /** Under [MediumMinWidth] wide, and narrow or short: a cover screen, a tiny phone. */
    Small,

    /** Two panes side by side, either side of any hinge. */
    Medium,

    /** Three panes, as on the desktops. Never across a fold. */
    Large,
}

/** Narrower than this is [WindowLayoutClass.Small]. */
val SmallBelowWidth = 340.dp

/** Shorter than this is [WindowLayoutClass.Small] if under [MediumMinWidth], and dense at any width. */
val ShortBelowHeight = 480.dp

/** From here across a window has two panes, whatever its height. */
val MediumMinWidth = 600.dp

/** A 10 inch tablet in portrait is 800 wide (Store_Tablet_10), and a 7 inch in portrait 600. */
val LargeMinWidth = 800.dp

/** A 7 inch tablet in landscape is 600 tall (Store_Tablet_7). */
val LargeMinHeight = 560.dp

/** A fold or hinge across a window, in the window's own dp: [start] to [end] is what it covers, along x if [vertical]. */
@Immutable
data class WindowFold(val vertical: Boolean, val start: Dp, val end: Dp)

/**
 * How much smaller text and sizes are drawn. One setting for all of a window's screens, from
 * [WindowLayout.dense] and not the layout class, as a Medium window can be dense too. [scaledBy]
 * applies it to everything under [ProvideWindowLayout], so a screen needs no code of its own for
 * this, only for what it should leave out or lay out differently.
 *
 * [size] keeps the 48dp touch target Compose gives buttons at 40dp or more of real size: don't
 * take it under 0.84 without raising LocalMinimumInteractiveComponentSize to match.
 */

@Immutable
data class LayoutScale(val font: Float, val size: Float) {
    companion object {
        val Regular = LayoutScale(font = 1f, size = 1f)
        val Dense = LayoutScale(font = 0.9f, size = 0.85f)
    }
}

/**
 * This density with [scale] applied. Text goes through the system's own conversion first, so that
 * its large text setting, which grows small text more than large, stays as the user has it, and
 * dense text is that much smaller than it would be.
 */
internal fun Density.scaledBy(scale: LayoutScale): Density =
    if (scale == LayoutScale.Regular) this else ScaledDensity(this, scale)

private data class ScaledDensity(val base: Density, val scale: LayoutScale) : Density {
    override val density get() = base.density * scale.size
    override val fontScale get() = base.fontScale * scale.font / scale.size

    override fun TextUnit.toDp(): Dp = with(base) { this@toDp.toDp() } * (scale.font / scale.size)

    override fun Dp.toSp(): TextUnit = with(base) { (this@toSp * (scale.size / scale.font)).toSp() }
}

/**
 * To the nearest dp, for comparing with the thresholds: a window's pixels over its density don't
 * come out whole, and 1440 at 1.8 is 799.99994.
 */
private fun Dp.whole(): Dp = value.roundToInt().dp

/**
 * The layout for a window [width] by [height]. Any [fold] rules out Large, as a hinge would cut
 * the middle pane in half. [foldKnown] is false if a fold can't be ruled out, which also rules it
 * out: on a device with a hinge, whose report can come late or not at all.
 */
fun windowLayoutClass(width: Dp, height: Dp, fold: WindowFold? = null, foldKnown: Boolean = true): WindowLayoutClass {
    val across = width.whole()
    val down = height.whole()
    return when {
        across >= LargeMinWidth && down >= LargeMinHeight && fold == null && foldKnown -> WindowLayoutClass.Large
        across >= MediumMinWidth -> WindowLayoutClass.Medium
        across < SmallBelowWidth || down < ShortBelowHeight -> WindowLayoutClass.Small
        else -> WindowLayoutClass.Compact
    }
}

/** What the window is, for choosing a layout: read it from [LocalWindowLayout]. */
@Immutable
data class WindowLayout(
    val layoutClass: WindowLayoutClass,
    val width: Dp,
    val height: Dp,
    val fold: WindowFold? = null,
) {
    /** Short, as a phone is in landscape: screens then put their headers' parts side by side. */
    val short: Boolean get() = height.whole() < ShortBelowHeight

    /** Space is tight: smaller text, a smaller header, the small mini player with no seekbar. */
    val dense: Boolean get() = layoutClass == WindowLayoutClass.Small || short

    /** The scaling that goes with [dense]. */
    val scale: LayoutScale get() = if (dense) LayoutScale.Dense else LayoutScale.Regular

    companion object {
        fun of(width: Dp, height: Dp, fold: WindowFold? = null, foldKnown: Boolean = true) =
            WindowLayout(windowLayoutClass(width, height, fold, foldKnown), width, height, fold)
    }
}

/**
 * A part of the window something is drawn in. [sides] are the window's edges it touches, the
 * only ones whose insets are its to clear: see [inPane]. Its sizes are in the dp of what's drawn,
 * which a dense window scales (see [LayoutScale]), so they compare with what's laid out in it
 * as they are. Only [WindowLayout] has the window's own dp, for the thresholds; [short] is the one
 * of those a pane needs.
 */
@Immutable
data class PaneArea(
    val x: Dp,
    val width: Dp,
    val height: Dp,
    val sides: WindowInsetsSides,
    val whole: Boolean = false,
    /** The window is short, as a phone is in landscape: see [isShortPane]. */
    val short: Boolean = false,
) {
    /** This area of a window at [layout]'s size, in the dp of what's drawn there. */
    internal fun drawnIn(layout: WindowLayout): PaneArea {
        val size = layout.scale.size
        return copy(x = x / size, width = width / size, height = height / size, short = layout.short)
    }
}

internal val AllSides = WindowInsetsSides.Horizontal + WindowInsetsSides.Vertical

/**
 * Where each part of the app goes. App.kt calls [Pane] once for each, at the same place in every
 * layout, and only these areas differ: that's what lets what's saved (a list's scroll position,
 * say) find its way back after a fold or unfold, as rememberSaveable's keys come from the call's
 * place in the composition. Keep it so: branching on the layout class around a [Pane] call
 * loses them.
 */
@Immutable
data class Panes(
    val main: PaneArea,
    val pages: PaneArea,
    val miniPlayer: PaneArea,
    val player: PaneArea,
    /** What home is drawn in: [main], unless it has [columns]. */
    val home: PaneArea = main,
    /** Where the categories go, with three panes: [main] is then the middle pane. */
    val categories: PaneArea? = null,
) {
    /** Pages open beside the main content rather than over it. */
    val sideBySide: Boolean get() = main != pages

    /** Home is columns across the window, not one list: see [HomeLayout]. */
    val columns: Boolean get() = home != main

    internal fun map(f: (PaneArea) -> PaneArea) =
        Panes(f(main), f(pages), f(miniPlayer), f(player), f(home), categories?.let(f))
}

/** The categories' share of a window, within [NarrowestCategories] and [WidestCategories]. */
private const val CATEGORIES_SHARE = 0.2f

/** As narrow as the categories go, in the narrowest Large window: their rows' text still fits. */
private val NarrowestCategories = 160.dp
private val WidestCategories = 208.dp

/** The pages' share of what the categories leave. */
private const val PAGES_SHARE = 0.45f

/** A track list's and the player's width at least, and at most, as they gain nothing past it. */
private val NarrowestPages = 320.dp
private val WidestPages = 560.dp

/**
 * The one place that decides where everything goes, for each layout class. Right-to-left, [rtl],
 * mirrors it: pages open on the left.
 */
fun panesFor(layout: WindowLayout, rtl: Boolean = false): Panes = windowPanes(layout, rtl).map { it.drawnIn(layout) }

/** [panesFor] in the window's dp. */
private fun windowPanes(layout: WindowLayout, rtl: Boolean): Panes = when (layout.layoutClass) {
    WindowLayoutClass.Compact, WindowLayoutClass.Small -> {
        val all = PaneArea(0.dp, layout.width, layout.height, AllSides, whole = true)
        Panes(all, all, all, all)
    }
    // Either side of a hinge, which has a width on some devices; else halves.
    WindowLayoutClass.Medium -> {
        val fold = layout.fold?.takeIf { it.vertical }
        val panes = split(layout, fold?.start ?: (layout.width / 2), fold?.end ?: (layout.width / 2), rtl)
        panes.copy(home = PaneArea(0.dp, layout.width, layout.height, AllSides, whole = true))
    }
    WindowLayoutClass.Large -> threePanes(layout, rtl)
}

/** The categories, the middle pane and the pages, from the left, or from the right in right-to-left. */
private fun threePanes(layout: WindowLayout, rtl: Boolean): Panes {
    val categories = (layout.width * CATEGORIES_SHARE).coerceIn(NarrowestCategories, WidestCategories)
    val rest = layout.width - categories
    val pages = (rest * PAGES_SHARE).coerceIn(NarrowestPages, WidestPages)
    val middle = rest - pages
    val left = WindowInsetsSides.Left + WindowInsetsSides.Vertical
    val right = WindowInsetsSides.Right + WindowInsetsSides.Vertical
    val main = PaneArea(if (rtl) pages else categories, middle, layout.height, WindowInsetsSides.Vertical)
    val pagesArea = PaneArea(if (rtl) 0.dp else categories + middle, pages, layout.height, if (rtl) left else right)
    val categoriesArea = PaneArea(if (rtl) pages + middle else 0.dp, categories, layout.height, if (rtl) right else left)
    return Panes(main, pagesArea, pagesArea, pagesArea, categories = categoriesArea)
}

private fun split(layout: WindowLayout, leftEnd: Dp, rightStart: Dp, rtl: Boolean): Panes {
    val left = PaneArea(0.dp, leftEnd, layout.height, WindowInsetsSides.Left + WindowInsetsSides.Vertical)
    val right = PaneArea(
        rightStart,
        layout.width - rightStart,
        layout.height,
        WindowInsetsSides.Right + WindowInsetsSides.Vertical,
    )
    return if (rtl) Panes(right, left, left, left) else Panes(left, right, right, right)
}

/** The window's layout. Set by [ProvideWindowLayout]. */
val LocalWindowLayout = compositionLocalOf { WindowLayout.of(400.dp, 800.dp) }

/** The area what's being drawn is in: the whole window, unless it's in a [Pane]. */
val LocalPane = compositionLocalOf { PaneArea(0.dp, 400.dp, 800.dp, AllSides, whole = true) }

/**
 * These insets as far as they reach this pane: a pane away from a window edge has nothing to
 * clear there. For what's drawn in a pane's composition, not in a dialog's or sheet's own window.
 */
@Composable
fun WindowInsets.inPane(): WindowInsets = only(LocalPane.current.sides)

/** Whether the pane is short, as a phone is in landscape: screens then put their headers' parts side by side. */
@Composable
fun isShortPane(): Boolean = LocalPane.current.short

/** [content] in [area], which [LocalPane] then says, at the area's place in the window. */
@Composable
fun Pane(area: PaneArea, content: @Composable BoxScope.() -> Unit) {
    CompositionLocalProvider(LocalPane provides area) {
        Box(
            if (area.whole) Modifier.fillMaxSize()
            // Measured from the left in any language: a Box would otherwise start from the right.
            else Modifier.fillMaxSize().wrapContentWidth(AbsoluteAlignment.Left)
                .absoluteOffset(x = area.x).width(area.width),
            content = content,
        )
    }
}

/**
 * A [Pane] for what only some layouts have, in [area] if there is one. It's always called, so what's
 * after it keeps its place in the composition: see [Panes].
 */
@Composable
fun SomePane(area: PaneArea?, content: @Composable BoxScope.() -> Unit) {
    if (area != null) Pane(area, content)
}

/**
 * [content] at the window's [LayoutScale], for inside a bottom sheet: it has a view of its own,
 * which goes back to the system's density, as a dialog's and a menu's do. Those are left at that,
 * as they fit and what's in them is short, and sheets' lists are what get long.
 */
@Composable
fun SheetScale(content: @Composable () -> Unit) {
    val scale = LocalWindowLayout.current.scale
    val density = LocalDensity.current
    CompositionLocalProvider(LocalDensity provides remember(density, scale) { density.scaledBy(scale) }, content = content)
}

/**
 * Keeps a fold for as long as the window stays the size it was seen at. On folding, the system
 * stops reporting the fold more than a second before the window shrinks, and in between an open
 * foldable would look like a tablet, and go Large.
 */
internal class FoldMemory<S> {
    private var size: S? = null
    private var fold: WindowFold? = null

    /** The fold for a window of [size], given the one the system [reported]. */
    fun resolve(size: S, reported: WindowFold?): WindowFold? {
        if (reported != null) {
            this.size = size
            fold = reported
        }
        return fold.takeIf { this.size == size }
    }
}

/**
 * Provides [LocalWindowLayout] and a whole-window [LocalPane], kept up with the live window and
 * its folds, and the [LocalDensity] of the window's [LayoutScale] for all that's drawn in it.
 */
@Composable
fun ProvideWindowLayout(content: @Composable () -> Unit) {
    val activity = LocalActivity.current
    val density = LocalDensity.current
    var folds by remember { mutableStateOf(emptyList<WindowFold>()) }
    LaunchedEffect(activity, density) {
        if (activity == null) return@LaunchedEffect
        WindowInfoTracker.getOrCreate(activity).windowLayoutInfo(activity).collect { info ->
            folds = info.displayFeatures.filterIsInstance<FoldingFeature>().map { feature ->
                val vertical = feature.orientation == FoldingFeature.Orientation.VERTICAL
                val bounds = feature.bounds
                with(density) {
                    if (vertical) WindowFold(true, bounds.left.toDp(), bounds.right.toDp())
                    else WindowFold(false, bounds.top.toDp(), bounds.bottom.toDp())
                }
            }
        }
    }
    // A process can go its whole life with no fold reported on a device that has one, as seen on
    // Test_Z_Fold_8, and the report takes about 450 ms to come where it does, so this is what
    // decides: a device with a hinge can't rule out a fold, and one without can from the first
    // frame, which keeps a tablet from starting as Medium every time it opens or turns.
    val hinged = remember(activity) {
        activity?.packageManager?.hasSystemFeature(PackageManager.FEATURE_SENSOR_HINGE_ANGLE) == true
    }
    val size = LocalWindowInfo.current.containerSize
    val memory = remember { FoldMemory<IntSize>() }
    val layout = with(density) {
        // A vertical fold first, as that's the one panes go either side of.
        val fold = memory.resolve(size, folds.firstOrNull { it.vertical } ?: folds.firstOrNull())
        WindowLayout.of(size.width.toDp(), size.height.toDp(), fold, foldKnown = !hinged)
    }
    LaunchedEffect(layout) { Log.i("kithsong-layout", "$layout dense=${layout.dense} hinged=$hinged") }
    val whole = PaneArea(0.dp, layout.width, layout.height, AllSides, whole = true).drawnIn(layout)
    val scaled = remember(density, layout.scale) { density.scaledBy(layout.scale) }
    CompositionLocalProvider(
        LocalWindowLayout provides layout,
        LocalPane provides whole,
        LocalDensity provides scaled,
        content = content,
    )
}
