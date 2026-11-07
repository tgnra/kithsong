package ng.kithso.android.ui

import android.content.ClipData
import android.content.Context
import android.view.View
import android.widget.Toast
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.draganddrop.dragAndDropTarget
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.awaitTouchSlopOrCancellation
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.draganddrop.DragAndDropEvent
import androidx.compose.ui.draganddrop.DragAndDropSourceModifierNode
import androidx.compose.ui.draganddrop.DragAndDropTarget
import androidx.compose.ui.draganddrop.DragAndDropTransferData
import androidx.compose.ui.draganddrop.toAndroidDragEvent
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.drawscope.ContentDrawScope
import androidx.compose.ui.graphics.layer.GraphicsLayer
import androidx.compose.ui.graphics.layer.drawLayer
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.PointerInputChange
import androidx.compose.ui.input.pointer.PointerInputScope
import androidx.compose.ui.input.pointer.PointerType
import androidx.compose.ui.input.pointer.SuspendingPointerInputModifierNode
import androidx.compose.ui.input.pointer.isPrimaryPressed
import androidx.compose.ui.input.pointer.isSecondaryPressed
import androidx.compose.ui.node.CompositionLocalConsumerModifierNode
import androidx.compose.ui.node.DelegatingNode
import androidx.compose.ui.node.DrawModifierNode
import androidx.compose.ui.node.LayoutAwareModifierNode
import androidx.compose.ui.node.ModifierNodeElement
import androidx.compose.ui.node.currentValueOf
import androidx.compose.ui.node.requireGraphicsContext
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.onLongClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.toSize
import ng.kithso.android.R
import ng.kithso.android.data.Addition
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Library
import ng.kithso.android.data.PlaylistEditor

// Dragging tracks onto playlists and albums onto album groups, in the tablet layout, as on the
// desktops (desktop/linux/src/drag.h). A track list's row drags its track, and an album's tile
// its album, as do Search's results. What takes them: a manual playlist's or album group's row
// in the sidebar's Recents and its tile on the Playlists or Album groups page, which add it as
// Add to playlist and Add to album group do. Smart lists take nothing, as they fill themselves.
//
// It's the platform's drag and drop, so a track or album dragged out to another app in split
// screen arrives there as its title and artist. Only this app knows what it is, from the drag's
// local state; text dragged in from elsewhere is refused.
//
// On a touch screen a long press then a move starts a drag, and a long press let go where it
// is opens the item's menu, as a long press always has; a mouse drags at once, as on the
// desktops, and a right-click opens the menu. The phone layout has no drag and drop: its long
// press is as it always was. TalkBack can't drag, but each item's menu has Add to playlist.

/**
 * What a drag carries: a track, for a manual playlist, or an album, for a manual album group.
 * [title] and [artist] are what another app gets.
 */
@Immutable
sealed interface DragItem {
    val uid: String
    val title: String
    val artist: String

    data class Track(override val uid: String, override val title: String, override val artist: String) : DragItem

    data class Album(override val uid: String, override val title: String, override val artist: String) : DragItem
}

/** The text another app gets for [this]: its title, then its artist after a dash if it has one. */
internal fun DragItem.text(): String = if (artist.isEmpty()) title else "$title — $artist"

/**
 * Whether [item] can be dropped on [ref]: a track on a manual playlist, one of [playlists], or
 * an album on a manual album group, one of [groups]. Anything else refuses it, as on the
 * desktops, so a smart list doesn't light up under it.
 */
internal fun canDrop(item: DragItem, ref: CollectionRef, playlists: Set<String>, groups: Set<String>): Boolean = when (item) {
    is DragItem.Track -> ref.kind == CollectionRef.Kind.Playlist && ref.uid in playlists
    is DragItem.Album -> ref.kind == CollectionRef.Kind.AlbumGroup && ref.uid in groups
}

/**
 * Drag and drop in the tablet layout: which playlists and album groups are manual, and so take
 * a drop, and what a drop does. It's [LocalDragDrop] there, and null elsewhere, which is how the
 * tiles and rows know whether they drag.
 */
@Stable
internal class DragDrop(private val editor: PlaylistEditor, private val context: Context) {
    // The manual playlists' and album groups' uids, read again after each change to them.
    private var playlists by mutableStateOf(emptySet<String>())
    private var groups by mutableStateOf(emptySet<String>())

    fun accepts(item: DragItem, ref: CollectionRef): Boolean = canDrop(item, ref, playlists, groups)

    /**
     * Adds [item] to [ref], called [name], as picking it in Add to playlist or Add to album
     * group does: the same change, which syncs, and the same toast saying where it went.
     */
    fun drop(item: DragItem, ref: CollectionRef, name: String) {
        if (!accepts(item, ref)) return
        val added = when (item) {
            is DragItem.Track -> {
                editor.addToPlaylist(ref.uid, Addition.Tracks(listOf(item.uid)))
                R.string.playlist_added
            }
            is DragItem.Album -> {
                editor.addToAlbumGroup(ref.uid, item.uid)
                R.string.album_group_added
            }
        }
        Toast.makeText(context, context.getString(added, name), Toast.LENGTH_SHORT).show()
    }

    suspend fun load() {
        editor.playlistChoices()?.let { list -> playlists = list.mapTo(HashSet()) { it.uid } }
        editor.albumGroupChoices()?.let { list -> groups = list.mapTo(HashSet()) { it.uid } }
    }
}

/** The tablet layout's [DragDrop], or null if the library can't be edited, as the made-up one can't. */
internal val LocalDragDrop = staticCompositionLocalOf<DragDrop?> { null }

/**
 * A [DragDrop] for [editor], which reads the manual lists again whenever [library] changes or
 * an edit is made: a smart list made or deleted changes the library, a playlist renamed or
 * emptied may not.
 */
@Composable
internal fun rememberDragDrop(editor: PlaylistEditor?, library: Library?): DragDrop? {
    val context = LocalContext.current.applicationContext
    val dragDrop = remember(editor, context) { editor?.let { DragDrop(it, context) } }
    LaunchedEffect(dragDrop, library) {
        if (dragDrop != null) editor?.edits?.collect { dragDrop.load() }
    }
    return dragDrop
}

/** What a drag in this app carries, or null for one from another app. */
private fun DragAndDropEvent.item(): DragItem? = toAndroidDragEvent().localState as? DragItem

/**
 * Makes this a place to drop on: the playlist or album group [ref], called [name]. It takes
 * only what [DragDrop.accepts], and while that's dragged over it it's marked, in [shape].
 * Outside the tablet layout it's nothing. It has no semantics: TalkBack's way to add is the
 * menu's Add to playlist.
 */
@Composable
internal fun Modifier.dropTarget(ref: CollectionRef, name: String, shape: Shape): Modifier {
    val dragDrop = LocalDragDrop.current ?: return this
    var over by remember { mutableStateOf(false) }
    val target = remember(dragDrop, ref, name) {
        object : DragAndDropTarget {
            override fun onEntered(event: DragAndDropEvent) {
                over = true
            }

            override fun onExited(event: DragAndDropEvent) {
                over = false
            }

            override fun onEnded(event: DragAndDropEvent) {
                over = false
            }

            override fun onDrop(event: DragAndDropEvent): Boolean {
                over = false
                val item = event.item() ?: return false
                if (!dragDrop.accepts(item, ref)) return false
                dragDrop.drop(item, ref, name)
                return true
            }
        }
    }
    val colors = MaterialTheme.colorScheme
    return this
        .dragAndDropTarget(
            shouldStartDragAndDrop = { event -> event.item()?.let { dragDrop.accepts(it, ref) } == true },
            target = target,
        )
        .then(if (over) Modifier.background(colors.primary.copy(alpha = 0.16f), shape).border(2.dp, colors.primary, shape) else Modifier)
}

/**
 * Tapping this calls [onClick], and a long press or a mouse's right-click opens its menu,
 * [onMenu], if it has one ([onMenuLabel] says what it is to TalkBack). In the tablet layout an
 * item that can be dragged ([drag]) drags as this file's top says, the long press opening the
 * menu only once it's let go; the haptic still comes as the long press is reached, so it can
 * be felt that the item's held.
 */
@Composable
internal fun Modifier.menuClickable(
    onMenu: (() -> Unit)?,
    onClick: () -> Unit,
    drag: DragItem? = null,
    enabled: Boolean = true,
    onClickLabel: String? = null,
    onMenuLabel: String? = null,
): Modifier {
    val dragging = drag != null && enabled && LocalDragDrop.current != null
    val rightClick = if (onMenu != null && enabled) Modifier.secondaryClick(onMenu) else Modifier
    return if (dragging && drag != null) {
        this
            .combinedClickable(enabled = enabled, onClickLabel = onClickLabel, onClick = onClick)
            // The long press is the drag's now, but TalkBack still offers it as the menu.
            .then(if (onMenu != null) Modifier.semantics { onLongClick(onMenuLabel) { onMenu(); true } } else Modifier)
            .then(DragSourceElement(drag, onMenu, MaterialTheme.colorScheme.surfaceContainerHigh))
            .then(rightClick)
    } else {
        this
            .combinedClickable(
                enabled = enabled,
                onClickLabel = onClickLabel,
                onLongClickLabel = onMenuLabel,
                onLongClick = onMenu,
                onClick = onClick,
            )
            .then(rightClick)
    }
}

/**
 * A mouse's right-click (the secondary button) calls [onClick] once it's let go, as a long
 * press opens a menu. It takes the click before anything else sees it, so the item isn't
 * opened or played too.
 */
internal fun Modifier.secondaryClick(onClick: () -> Unit): Modifier = this then SecondaryClickElement(onClick)

private data class SecondaryClickElement(val onClick: () -> Unit) : ModifierNodeElement<SecondaryClickNode>() {
    override fun create() = SecondaryClickNode(onClick)

    override fun update(node: SecondaryClickNode) {
        node.onClick = onClick
    }
}

private class SecondaryClickNode(var onClick: () -> Unit) : DelegatingNode() {
    init {
        delegate(
            SuspendingPointerInputModifierNode {
                awaitEachGesture {
                    // The first pass, which comes to this before the clickable's, so it never
                    // sees the press.
                    val down = awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
                    if (down.type != PointerType.Mouse || !currentEvent.buttons.isSecondaryPressed) return@awaitEachGesture
                    down.consume()
                    do {
                        val event = awaitPointerEvent(PointerEventPass.Initial)
                        event.changes.forEach { it.consume() }
                    } while (event.changes.any { it.pressed })
                    onClick()
                }
            },
        )
    }
}

private data class DragSourceElement(
    val item: DragItem,
    val onMenu: (() -> Unit)?,
    val shadowColor: Color,
) : ModifierNodeElement<DragSourceNode>() {
    override fun create() = DragSourceNode(item, onMenu, shadowColor)

    override fun update(node: DragSourceNode) {
        node.item = item
        node.onMenu = onMenu
        node.shadowColor = shadowColor
    }
}

/**
 * Drags [item] by the platform's drag and drop, with what's drawn here as the shadow under the
 * finger or pointer, on [shadowColor]: rows are clear, for the page's background, which the
 * shadow leaves behind. It goes after the item's clickable, so it sees each move and the lift
 * before the clickable and the lists and pages around it do, and takes those it uses: once a
 * touch is held long enough nothing scrolls or swipes, and the lift that opens the menu isn't
 * also a tap. Before that it takes nothing, so a touch that moves first is a scroll, and one
 * let go first a tap.
 */
private class DragSourceNode(var item: DragItem, var onMenu: (() -> Unit)?, var shadowColor: Color) :
    DelegatingNode(),
    DrawModifierNode,
    LayoutAwareModifierNode,
    CompositionLocalConsumerModifierNode {
    private var size = IntSize.Zero

    // What was last drawn here, to draw again as the shadow.
    private var shadow: GraphicsLayer? = null

    private val source = delegate(
        DragAndDropSourceModifierNode {
            val layer = shadow
            startDragAndDropTransfer(
                DragAndDropTransferData(
                    ClipData.newPlainText(item.title, item.text()),
                    localState = item,
                    // Other apps may take it too, as text.
                    flags = View.DRAG_FLAG_GLOBAL,
                ),
                size.toSize(),
            ) {
                drawRoundRect(shadowColor, cornerRadius = CornerRadius(ShadowCorner.toPx()))
                if (layer != null) drawLayer(layer)
            }
        },
    )

    init {
        delegate(SuspendingPointerInputModifierNode { detectDrag() })
    }

    override fun onAttach() {
        shadow = requireGraphicsContext().createGraphicsLayer()
    }

    override fun onDetach() {
        shadow?.let { requireGraphicsContext().releaseGraphicsLayer(it) }
        shadow = null
    }

    override fun onRemeasured(size: IntSize) {
        this.size = size
    }

    override fun ContentDrawScope.draw() {
        val layer = shadow
        if (layer == null) {
            drawContent()
            return
        }
        layer.record { this@draw.drawContent() }
        drawLayer(layer)
    }

    private suspend fun PointerInputScope.detectDrag() = awaitEachGesture {
        val down = awaitFirstDown(requireUnconsumed = false)
        val slop = viewConfiguration.touchSlop
        // A mouse drags as soon as it moves, as on the desktops; its right button is the menu's.
        if (down.type == PointerType.Mouse) {
            if (!currentEvent.buttons.isPrimaryPressed) return@awaitEachGesture
            awaitTouchSlopOrCancellation(down.id) { change, _ -> change.consume() } ?: return@awaitEachGesture
            source.requestDragAndDropTransfer(down.position)
            return@awaitEachGesture
        }
        // Until the long press: let go, moved or taken by a scroll first, it's not for this.
        val interrupted = withTimeoutOrNull(viewConfiguration.longPressTimeoutMillis) {
            var change: PointerInputChange
            do {
                change = awaitPointerEvent().changes.firstOrNull { it.id == down.id } ?: break
            } while (change.pressed && !change.isConsumed && (change.position - down.position).getDistance() <= slop)
            true
        }
        if (interrupted != null) return@awaitEachGesture
        currentValueOf(LocalHapticFeedback).performHapticFeedback(HapticFeedbackType.LongPress)
        while (true) {
            val change = awaitPointerEvent().changes.firstOrNull { it.id == down.id } ?: return@awaitEachGesture
            if (change.isConsumed) return@awaitEachGesture
            if (!change.pressed) {
                // Without a menu, the lift is the tap it always was.
                val menu = onMenu ?: return@awaitEachGesture
                change.consume()
                menu()
                return@awaitEachGesture
            }
            change.consume()
            if ((change.position - down.position).getDistance() > slop) {
                // Where it was pressed, which is sure to be on the item.
                source.requestDragAndDropTransfer(down.position)
                return@awaitEachGesture
            }
        }
    }
}

/** The corners of a drag's shadow, as a tile's. */
private val ShadowCorner = 12.dp
