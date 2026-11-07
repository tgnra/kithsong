package ng.kithso.android.ui

import androidx.compose.foundation.focusGroup
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.focusRestorer
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.drawOutline
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.translate
import androidx.compose.ui.input.InputMode
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.isAltPressed
import androidx.compose.ui.input.key.isCtrlPressed
import androidx.compose.ui.input.key.isMetaPressed
import androidx.compose.ui.input.key.isShiftPressed
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalInputModeManager
import androidx.compose.ui.unit.dp

// Moving about with the keyboard. Compose moves the focus with Tab and the arrows by itself,
// Enter clicks what has it, and Material's state layers show hovering; this adds what it
// doesn't do: a focus ring that shows on album art, the menu key, Space playing from a tile
// as from the desktops' album grids, and in the tablet layout, Tab going from one part of the
// window to the next rather than through every tile (focusArea(), TabExit).

/** The window's [KeyboardShortcuts], for Space on a tile or row, which takes the key before its click would. */
val LocalShortcuts = staticCompositionLocalOf<KeyboardShortcuts?> { null }

/**
 * Where Tab and Shift+Tab go from a tile or row: in the tablet layout, a list is one stop, as on
 * the desktops, and Tab goes on to the next part of the window. Elsewhere, null, they go from
 * one item to the next as usual.
 */
@Immutable
class TabExit(val next: () -> Unit, val previous: () -> Unit)

val LocalTabExit = staticCompositionLocalOf<TabExit?> { null }

/** How wide the focus ring is. */
private val FocusRingWidth = 3.dp

/**
 * A ring inside [shape] while what follows has the keyboard's focus. Material only tints what's
 * focused, which album art hides. Clickable things only take the focus from the keyboard, so
 * touch never shows it.
 */
@Composable
internal fun Modifier.focusRing(shape: Shape): Modifier {
    var focused by remember { mutableStateOf(false) }
    val color = MaterialTheme.colorScheme.primary
    return onFocusChanged { focused = it.isFocused }
        .drawWithContent {
            drawContent()
            if (focused) {
                val width = FocusRingWidth.toPx()
                // Inside the edge, so a clip around it doesn't cut half of it away.
                translate(width / 2, width / 2) {
                    val outline = shape.createOutline(Size(size.width - width, size.height - width), layoutDirection, this)
                    drawOutline(outline, color, style = Stroke(width))
                }
            }
        }
}

/**
 * The keys of a tile or row, put just before its clickable, which takes Enter itself: the
 * [focusRing], the menu key and Shift+F10 for [onMenu], the long press's menu, Space to play
 * or pause, as in the desktops' album grids, rather than open it, and Tab out of the list
 * where there's a [TabExit]. The menu key is the press of a key rather than a gesture, so it's
 * here, apart from the long press and the right-click.
 */
@Composable
internal fun Modifier.listItemKeys(shape: Shape, onMenu: (() -> Unit)?): Modifier {
    val shortcuts = LocalShortcuts.current
    val tab = LocalTabExit.current
    val menu by rememberUpdatedState(onMenu)
    return focusRing(shape).onPreviewKeyEvent { event ->
        val down = event.type == KeyEventType.KeyDown
        val plain = !event.isCtrlPressed && !event.isAltPressed && !event.isMetaPressed
        val open = menu
        when {
            // Both the press and the release, so the click doesn't see half of one.
            open != null && plain && (event.key == Key.Menu || (event.key == Key.F10 && event.isShiftPressed)) -> {
                if (down && event.nativeKeyEvent.repeatCount == 0) open()
                true
            }
            event.key == Key.Spacebar && plain && !event.isShiftPressed && shortcuts != null -> {
                if (down && event.nativeKeyEvent.repeatCount == 0) shortcuts.run(Shortcut.PlayPause)
                true
            }
            event.key == Key.Tab && plain && tab != null -> {
                if (down) if (event.isShiftPressed) tab.previous() else tab.next()
                true
            }
            else -> false
        }
    }
}

/** Focuses what this asks for, if it's there to be focused. */
internal fun FocusRequester.focusIfThere() {
    // It throws if nothing is attached, as when the part it's for has just gone.
    runCatching { requestFocus() }
}

/**
 * Makes what follows a part of the window the keyboard's focus goes to as a whole, as the
 * tablet layout's top bar, sidebar, content, pane and pages are. Focusing [requester] goes back
 * to what in it last had the focus, or else to its first item. While [covered], by a page over
 * it, the focus can't come into it, by the arrows or by Tab, from what's over it.
 *
 * With [focusWhenShown], it takes the focus when it's first shown, if the keyboard is in use,
 * as a page opening over another does, so the arrows don't go on moving about beneath it. If it
 * goes while it has the focus with the keyboard in use, [onGone] is called to put the focus
 * somewhere else; it's called as this goes, so what it focuses should wait a frame to be there.
 */
@Composable
internal fun Modifier.focusArea(
    requester: FocusRequester,
    covered: Boolean = false,
    focusWhenShown: Boolean = false,
    onGone: (() -> Unit)? = null,
): Modifier {
    val inputMode = LocalInputModeManager.current
    val focus = remember { FocusHolder() }
    val gone by rememberUpdatedState(onGone)
    if (focusWhenShown) {
        LaunchedEffect(Unit) {
            if (inputMode.inputMode == InputMode.Keyboard) {
                withFrameNanos {}
                requester.focusIfThere()
            }
        }
    }
    DisposableEffect(Unit) {
        onDispose { if (focus.has && inputMode.inputMode == InputMode.Keyboard) gone?.invoke() }
    }
    // Two groups: the outer one turns the focus away while covered, the inner one remembers
    // where in it the focus was. Each sets what entering it does, which one group can't do twice.
    return focusProperties { onEnter = { if (covered) cancelFocusChange() } }
        .focusGroup()
        .onFocusChanged { focus.has = it.hasFocus }
        .focusRequester(requester)
        .focusRestorer()
        .focusGroup()
}

/** Whether a [focusArea] has the focus, for when it goes; nothing draws from it. */
private class FocusHolder {
    var has = false
}

/** The focus ring's shape on a list's row, which has none of its own. */
internal val ListRowFocusShape = RoundedCornerShape(8.dp)
