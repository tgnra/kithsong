package ng.kithso.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.BoxWithConstraintsScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.sizeIn
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.IconButtonDefaults
import androidx.compose.material3.IconToggleButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.layout.Placeable
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.onLongClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt
import ng.kithso.android.R
import ng.kithso.android.data.Track
import ng.kithso.android.playback.Playback
import ng.kithso.android.ui.art.AlbumArt
import ng.kithso.android.ui.theme.ArtBackdrop
import ng.kithso.android.ui.theme.rememberArtColors

/** "Artist - Album", under a track's title; just the artist for a track on no album. */
@Composable
internal fun trackByline(artist: String, album: String) =
    if (album.isEmpty()) artist else stringResource(R.string.track_byline, artist, album)

@Composable
private fun Track.byline() = trackByline(artist, album)

/**
 * The floating player above the content: art, title and byline, play/pause, and a seekbar
 * beneath. Tapping it calls [onOpen] and a long press or a right-click [onMenu]; swiping it
 * left or right skips a track (see [TrackSwipeState]), which accessibility services get as
 * actions.
 */
@Composable
fun MiniPlayer(playback: Playback, track: Track, onOpen: () -> Unit, onMenu: () -> Unit, modifier: Modifier = Modifier) {
    val swipe = rememberTrackSwipe(playback)
    val nextLabel = stringResource(R.string.next_track)
    val previousLabel = stringResource(R.string.previous_track)
    Surface(
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        tonalElevation = 3.dp,
        shadowElevation = 6.dp,
        modifier = modifier,
    ) {
        Column(
            Modifier
                .verticalDragEndsPress()
                .trackSwipe(swipe)
                .semantics {
                    customActions = listOfNotNull(
                        CustomAccessibilityAction(nextLabel) { playback.next(); true }.takeIf { playback.hasNext },
                        CustomAccessibilityAction(previousLabel) { playback.skipToPrevious(); true }.takeIf { playback.hasPrevious },
                    )
                }
                .combinedClickable(
                    onClickLabel = stringResource(R.string.open_player),
                    onLongClickLabel = stringResource(R.string.track_menu),
                    onLongClick = onMenu,
                    onClick = onOpen,
                )
                .secondaryClick(onMenu)
                .padding(start = 10.dp, end = 4.dp, top = 10.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                TrackSlides(swipe, Modifier.weight(1f).clipToBounds()) { shown ->
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        TrackArt(shown, contentDescription = null, shape = RoundedCornerShape(8.dp), modifier = Modifier.size(48.dp))
                        Column(Modifier.weight(1f).padding(horizontal = 12.dp)) {
                            Text(
                                shown?.title.orEmpty(),
                                style = MaterialTheme.typography.titleSmall,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                            Text(
                                shown?.byline().orEmpty(),
                                style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                            )
                        }
                    }
                }
                PlayPause(playback)
            }
            Seekbar(
                positionMs = { playback.positionMs },
                durationMs = track.durationMs,
                onSeek = playback::seekTo,
                modifier = Modifier.fillMaxWidth().padding(end = 6.dp).height(28.dp),
                trackHeight = 4.dp,
                thumbHeight = 14.dp,
            )
        }
    }
}

/** [track]'s art, or for a track not known yet the plain background art loads over. */
@Composable
private fun TrackArt(track: Track?, contentDescription: String?, shape: Shape, modifier: Modifier) {
    if (track != null) AlbumArt(track.art, contentDescription, shape, modifier)
    else Box(modifier.clip(shape).background(MaterialTheme.colorScheme.surfaceVariant))
}

@Composable
private fun PlayPause(playback: Playback) {
    IconButton(onClick = playback::toggle) {
        if (playback.isPlaying) Icon(Icons.Pause, contentDescription = stringResource(R.string.pause))
        else Icon(Icons.Play, contentDescription = stringResource(R.string.play))
    }
}

/**
 * The full player, over a background coloured from the art: large art, then title and byline, seekbar right under them, and play/pause
 * between previous and next, with Add to playlist ([onAddToPlaylist], disabled if null) and
 * the shuffle toggle at the ends. [onCollapse] slides it away; it is also dismissed by swiping
 * down (see [DismissibleScreen]). Swiping left or right skips a track, moving the art and title
 * (see [TrackSwipeState]). [onMenu], if given, opens the track's menu from a button across from
 * the collapse button, and from a long press on the art.
 *
 * The chevron facing up at the foot of the player, away from the one in the corner that closes
 * it, slides the view up to show the queue ([QueuePage]), as does dragging up, and [slide] holds
 * how far. A window too short to spare the height has that chevron in the middle of the top row.
 * Only the player's own view swipes between tracks, and the background stays put under both.
 * [queueBackEnabled]: system back is the queue's while it's open.
 *
 * Half open in tabletop, it's laid out around the fold instead (see [TabletopPlayerLayout]):
 * the art and title in the half that stands up, and the controls in the half on the table, with
 * the queue beside them where there's room for it, so it's never slid to.
 */
@Composable
internal fun FullPlayer(
    playback: Playback,
    track: Track,
    slide: QueueSlide,
    queueBackEnabled: Boolean,
    onCollapse: () -> Unit,
    onAddToPlaylist: (() -> Unit)? = null,
    onMenu: (() -> Unit)? = null,
) {
    val swipe = rememberTrackSwipe(playback)
    val shape = rememberWindowShape()
    QueueBackHandler(slide, queueBackEnabled)
    Surface(
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxSize().onSizeChanged { slide.page = it.height.toFloat() },
    ) {
        ArtBackdrop(rememberArtColors(track.art), Modifier.fillMaxSize())
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val fold = tabletopFold(shape)
            val queueBeside = fold?.queueBeside == true
            // The queue that was slid to is beside the controls now, and stays there.
            LaunchedEffect(queueBeside) { if (queueBeside) slide.hide() }
            BoxWithConstraints(
                Modifier
                    .fillMaxSize()
                    .graphicsLayer { translationY = -slide.progress * slide.page }
                    .coveredIf(slide.open)
                    .then(if (queueBeside) Modifier else Modifier.queueDrag(slide, swipe))
                    .trackSwipe(swipe)
                    .windowInsetsPadding(WindowInsets.safeDrawing),
            ) {
                val wide = maxWidth > maxHeight
                // Below about this, two lines and the controls don't fit, so a short title gives its space back.
                val titleSpace = maxHeight >= 256.dp + 56.dp * LocalDensity.current.fontScale
                if (fold != null) {
                    TabletopPlayerLayout(
                        fold,
                        topRow = { PlayerTopRow(playback, onCollapse, onMenu, chevron = null) },
                        art = { PlayerArt(swipe, onMenu, Modifier) },
                        title = {
                            PlayerTitle(swipe, twoLineSpace = true, if (fold.titleBeside) Alignment.CenterStart else Alignment.TopStart)
                        },
                        controls = { PlayerControls(playback, track, onAddToPlaylist) },
                        // Dragging on the list scrolls it, and nothing else.
                        side = {
                            if (queueBeside) QueuePage(playback, slide = null, Modifier.keepsDrags())
                            else QueueChevron(slide)
                        },
                        modifier = Modifier.fillMaxSize(),
                    )
                } else Column(Modifier.fillMaxSize().padding(horizontal = 24.dp)) {
                    PlayerTopRow(playback, onCollapse, onMenu, chevron = if (wide) ({ QueueChevron(slide) }) else null)
                    if (wide) {
                        Row(Modifier.fillMaxSize().padding(bottom = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                            PlayerArt(swipe, onMenu, Modifier.fillMaxHeight())
                            Spacer(Modifier.size(32.dp))
                            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(ControlsGap)) {
                                PlayerTitle(swipe, titleSpace, Alignment.CenterStart)
                                // Moved, not closed up, so this centred column keeps its height and the title stays put.
                                PlayerControls(playback, track, onAddToPlaylist, Modifier.offset(y = -ControlsGap))
                            }
                        }
                    } else {
                        TallPlayerLayout(
                            art = { PlayerArt(swipe, onMenu, Modifier) },
                            title = { PlayerTitle(swipe, titleSpace, Alignment.TopStart) },
                            controls = { PlayerControls(playback, track, onAddToPlaylist) },
                            chevron = { QueueChevron(slide) },
                            modifier = Modifier.weight(1f),
                        )
                    }
                }
            }
            if (slide.shown && !queueBeside) {
                QueuePage(
                    playback,
                    slide,
                    Modifier
                        .fillMaxSize()
                        .graphicsLayer { translationY = (1f - slide.progress) * slide.page }
                        .queueDrag(slide)
                        .windowInsetsPadding(WindowInsets.safeDrawing),
                )
            }
        }
    }
}

/**
 * The close button, the menu button across from it if there's [onMenu], with the cast button
 * ([CastButton]) before it where [Playback.cast] is available, and between them the [chevron]
 * to the queue, where the window is too short for it at the foot.
 */
@Composable
private fun PlayerTopRow(playback: Playback, onCollapse: () -> Unit, onMenu: (() -> Unit)?, chevron: (@Composable () -> Unit)?) {
    // Nudged so the icons, not their touch targets, line up with the art. The ends share the
    // width evenly, so the chevron stays in the middle with two buttons at one end.
    Row(Modifier.fillMaxWidth().padding(top = 4.dp)) {
        Row(Modifier.weight(1f)) {
            IconButton(onClick = onCollapse, modifier = Modifier.offset(x = (-12).dp)) {
                Icon(Icons.ExpandMore, contentDescription = stringResource(R.string.close_player))
            }
        }
        chevron?.invoke()
        Row(Modifier.weight(1f), horizontalArrangement = Arrangement.End) {
            // Nudged with the menu button, so their touch targets still meet.
            if (playback.cast.available) CastButton(playback.cast, Modifier.offset(x = 12.dp))
            if (onMenu != null) IconButton(onClick = onMenu, modifier = Modifier.offset(x = 12.dp)) {
                Icon(Icons.MoreVert, contentDescription = stringResource(R.string.track_menu))
            }
        }
    }
}

/** Slides the view to the queue. */
@Composable
private fun QueueChevron(slide: QueueSlide) {
    IconButton(onClick = slide::show) {
        Icon(Icons.ExpandLess, contentDescription = stringResource(R.string.open_queue))
    }
}

/**
 * The full player's parts down a window taller than it's wide. The art is as big as it can be
 * in the height the rest leaves. Height left over from that goes above the art and below the
 * controls, and past their limits, around the lot.
 *
 * The title sits under the art and the controls right under the title, which keeps the height
 * of two lines, so moving between a short title and a long one moves none of it. The chevron
 * stays at the foot, in the room below the controls.
 */
@Composable
private fun TallPlayerLayout(
    art: @Composable () -> Unit,
    title: @Composable () -> Unit,
    controls: @Composable () -> Unit,
    chevron: @Composable () -> Unit,
    modifier: Modifier,
) {
    Layout(listOf(art, title, controls, chevron), modifier) { (artItem, titleItem, controlsItem, chevronItem), constraints ->
        val width = constraints.maxWidth
        val height = constraints.maxHeight
        val artGap = ArtGap.roundToPx()
        val chevronPlaced = chevronItem.single().measure(Constraints())
        val bottom = maxOf(BottomGap.roundToPx(), chevronPlaced.height - ControlsGap.roundToPx())
        // ControlsGap isn't placed, it's part of the room below the controls, which sizes the art.
        val fixed = artGap + ControlsGap.roundToPx() + bottom
        val titlePlaced = titleItem.single().measure(Constraints(width, width, 0, height))
        // The controls come first, as they shrink only when there's no other way.
        val controlsPlaced = controlsItem.single()
            .measure(Constraints(width, width, 0, (height - titlePlaced.height - fixed).coerceAtLeast(0)))
        val room = height - titlePlaced.height - controlsPlaced.height - fixed
        val side = minOf(width, MaxArtSize.roundToPx(), room).coerceAtLeast(0)
        val artPlaced = artItem.single().measure(Constraints.fixed(side, side))

        val spare = (room - side).coerceAtLeast(0)
        val aboveArt = minOf(spare * 3 / 5, MaxGapAboveArt.roundToPx())
        val belowControls = minOf(spare * 2 / 5, MaxGapBelowControls.roundToPx())
        val around = (spare - aboveArt - belowControls) / 2
        layout(width, height) {
            val artTop = around + aboveArt
            val titleTop = artTop + side + artGap
            artPlaced.place((width - side) / 2, artTop)
            titlePlaced.place(0, titleTop)
            controlsPlaced.place(0, titleTop + titlePlaced.height)
            chevronPlaced.place((width - chevronPlaced.width) / 2, height - chevronPlaced.height)
        }
    }
}

/**
 * Where the fold crosses the full player's content in tabletop, in pixels down from the top of
 * the content, inside the system bars: [top] is where the half that stands up ends and [bottom]
 * where the half on the table begins, which are the same for a fold with no width.
 * [titleBeside] is whether the half that stands up is wide enough for the title beside the art,
 * and [queueBeside] whether the half on the table has room for the queue beside the controls.
 */
@Immutable
private class TabletopFold(val top: Int, val bottom: Int, val titleBeside: Boolean, val queueBeside: Boolean)

/**
 * The fold across this player, in tabletop, if each half has room for its part. The thresholds
 * are on the content inside the system bars, so the navigation bar's height counts against the
 * half on the table.
 *
 * The fold's bounds are in the window, and the player is placed against them only while it fills
 * the window, so that the window's origin is its own and the content is moved from it by the
 * insets alone. The bounds aren't mapped through the player's own position instead, as that
 * moves as the player slides in and out, which would move the art and title against the screen.
 */
@Composable
private fun BoxWithConstraintsScope.tabletopFold(shape: WindowShape): TabletopFold? {
    val fold = shape.fold?.takeIf { shape.tabletop } ?: return null
    val window = LocalWindowInfo.current.containerSize
    if (constraints.maxWidth != window.width || constraints.maxHeight != window.height) return null
    val density = LocalDensity.current
    val insets = WindowInsets.safeDrawing
    val insetTop = insets.getTop(density)
    val height = window.height - insetTop - insets.getBottom(density)
    val width = window.width - insets.getLeft(density, LocalLayoutDirection.current) -
        insets.getRight(density, LocalLayoutDirection.current)
    val top = fold.bounds.top.roundToInt() - insetTop
    val bottom = fold.bounds.bottom.roundToInt() - insetTop
    return with(density) {
        val minHalf = MinTabletopHalf.roundToPx()
        if (top < minHalf || height - bottom < minHalf) return null
        val inner = width - 2 * PlayerGutter.roundToPx()
        // Half as wide again as it's tall, the title has room beside art the half's height.
        val titleBeside = inner > (top - TabletopGap.roundToPx()) * 3 / 2
        val queueBeside = inner >= (MinTabletopControls + TabletopGap * 2 + MinTabletopQueue).roundToPx() &&
            height - bottom >= MinTabletopQueueHeight.roundToPx()
        TabletopFold(top, bottom, titleBeside, queueBeside)
    }
}

/**
 * The full player's parts around a fold in tabletop. Nothing crosses the fold: [topRow], [art]
 * and [title] go above it, and [controls] and [side] below, each kept [TabletopGap] from it.
 *
 * Above, the art is as big as the height left by the top row and the title. A half that's wide
 * for its height, as a book-style foldable's is, has the title beside the art
 * ([TabletopFold.titleBeside]); a narrower one, as a flip phone's, has it under the art.
 *
 * Below, with [TabletopFold.queueBeside], the controls are down the left of the half and [side]
 * is the queue down the right, to the edge, as its rows have their own margins. Without, the
 * controls take the half's width and [side] is the chevron to the queue, at the foot as in
 * [TallPlayerLayout].
 */
@Composable
private fun TabletopPlayerLayout(
    fold: TabletopFold,
    topRow: @Composable () -> Unit,
    art: @Composable () -> Unit,
    title: @Composable () -> Unit,
    controls: @Composable () -> Unit,
    side: @Composable () -> Unit,
    modifier: Modifier,
) {
    Layout(
        contents = listOf(topRow, art, title, controls, side),
        modifier = modifier,
    ) { (topRowItem, artItem, titleItem, controlsItem, sideItem), constraints ->
        val width = constraints.maxWidth
        val height = constraints.maxHeight
        val gutter = PlayerGutter.roundToPx()
        val gap = TabletopGap.roundToPx()
        val inner = (width - 2 * gutter).coerceAtLeast(0)
        val maxArt = MaxArtSize.roundToPx()

        // Above the fold.
        val topRowPlaced = topRowItem.single().measure(Constraints(inner, inner, 0, fold.top))
        val upperTop = topRowPlaced.height
        val upperHeight = (fold.top - gap - upperTop).coerceAtLeast(0)
        val titleBeside = fold.titleBeside
        val artSide: Int
        val titlePlaced = if (titleBeside) {
            artSide = minOf(upperHeight, maxArt, inner / 2)
            val titleWidth = (inner - artSide - gap * 2).coerceAtLeast(0)
            titleItem.single().measure(Constraints(titleWidth, titleWidth, 0, upperHeight))
        } else {
            val placed = titleItem.single().measure(Constraints(inner, inner, 0, upperHeight))
            artSide = minOf(inner, maxArt, upperHeight - placed.height - ArtGap.roundToPx()).coerceAtLeast(0)
            placed
        }
        val artPlaced = artItem.single().measure(Constraints.fixed(artSide, artSide))

        // Below it.
        val lowerTop = fold.bottom + gap
        val controlsWidth = if (fold.queueBeside) minOf((inner - gap * 2) / 2, maxArt) else inner
        val sidePlaced: Placeable
        val lowerBottom: Int
        if (fold.queueBeside) {
            val sideWidth = (width - gutter - controlsWidth - gap * 2).coerceAtLeast(0)
            sidePlaced = sideItem.single().measure(Constraints.fixed(sideWidth, (height - fold.bottom).coerceAtLeast(0)))
            lowerBottom = height - BottomGap.roundToPx()
        } else {
            sidePlaced = sideItem.single().measure(Constraints())
            lowerBottom = height - maxOf(BottomGap.roundToPx(), sidePlaced.height)
        }
        val lowerHeight = (lowerBottom - lowerTop).coerceAtLeast(0)
        val controlsPlaced = controlsItem.single().measure(Constraints(controlsWidth, controlsWidth, 0, lowerHeight))

        layout(width, height) {
            topRowPlaced.place(gutter, 0)
            if (titleBeside) {
                artPlaced.place(gutter, upperTop + (upperHeight - artSide) / 2)
                titlePlaced.place(gutter + artSide + gap * 2, upperTop + (upperHeight - titlePlaced.height) / 2)
            } else {
                val block = artSide + ArtGap.roundToPx() + titlePlaced.height
                val artTop = upperTop + (upperHeight - block) / 2
                artPlaced.place(gutter + (inner - artSide) / 2, artTop)
                titlePlaced.place(gutter, artTop + artSide + ArtGap.roundToPx())
            }
            controlsPlaced.place(gutter, lowerTop + (lowerHeight - controlsPlaced.height) / 2)
            if (fold.queueBeside) sidePlaced.place(width - sidePlaced.width, fold.bottom)
            else sidePlaced.place((width - sidePlaced.width) / 2, height - sidePlaced.height)
        }
    }
}

/** The largest the full player's art gets, and the width of the buttons under it. */
private val MaxArtSize = 480.dp

/**
 * Under the art, under the controls at the least, and between the seekbar and the buttons. The
 * seekbar's touch target leaves 12dp above its thumb, which is the gap under the title.
 */
private val ArtGap = 24.dp
private val BottomGap = 24.dp
private val ControlsGap = 16.dp

/** Either side of the full player's content, but for the queue beside it in tabletop. */
private val PlayerGutter = 24.dp

/**
 * In tabletop: between the fold and what's either side of it, and twice that between the
 * controls and the queue beside them.
 */
private val TabletopGap = 16.dp

/**
 * The least height of each half, inside the system bars, for the tabletop layout: the top row,
 * some art and the title above, and the controls and the chevron below. Less, and the player
 * keeps its usual layout, across the fold.
 */
private val MinTabletopHalf = 220.dp

/**
 * The least width the controls and the queue each need to go side by side under the fold. The
 * controls' row of five buttons is about 290dp at the narrowest, and a queue row narrower than
 * this has little room left for its title between the handle, the time and the x. Book-style
 * foldables are about 840dp across in tabletop, so they have the queue beside; a flip phone is
 * about 410dp, so it has the controls alone and the chevron to the queue, as on a phone.
 */
private val MinTabletopControls = 320.dp
private val MinTabletopQueue = 280.dp

/** The least height under the fold for the queue beside the controls: its heading and two rows. */
private val MinTabletopQueueHeight = 200.dp

/** The most of what a tall window leaves over that goes above the art and below the controls. */
private val MaxGapAboveArt = 96.dp
private val MaxGapBelowControls = 64.dp

/**
 * [onMenu], if given, is a long press on the art, with the buzz a long press has elsewhere, or
 * a right-click.
 * It leaves taps alone, as there's nothing to tap, and a drag is the player's: its swipes take
 * the touch away from a press, and a press that's held takes the touch for itself.
 */
@Composable
private fun PlayerArt(swipe: TrackSwipeState, onMenu: (() -> Unit)?, modifier: Modifier) {
    val menu by rememberUpdatedState(onMenu)
    val haptics = LocalHapticFeedback.current
    val menuLabel = stringResource(R.string.track_menu)
    val longPress = if (onMenu == null) Modifier else Modifier
        .semantics(mergeDescendants = true) { onLongClick(menuLabel) { menu?.invoke(); true } }
        .secondaryClick { menu?.invoke() }
        .pointerInput(Unit) {
            detectTapGestures(
                onLongPress = {
                    haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                    menu?.invoke()
                },
            )
        }
    TrackSlides(swipe, modifier.widthIn(max = MaxArtSize).aspectRatio(1f).then(longPress)) { shown ->
        TrackArt(
            shown,
            contentDescription = shown?.let { stringResource(R.string.cover_of, it.album.ifEmpty { it.title }) },
            shape = RoundedCornerShape(24.dp),
            modifier = Modifier.fillMaxSize(),
        )
    }
}

/** The title and byline. With [twoLineSpace], always that height, with a shorter one at [alignment]. */
@Composable
private fun PlayerTitle(swipe: TrackSwipeState, twoLineSpace: Boolean, alignment: Alignment) {
    TrackSlides(swipe) { shown ->
        Box(Modifier.fillMaxWidth(), contentAlignment = alignment) {
            // Empty text holds the height, as large text doesn't scale its line height evenly.
            if (twoLineSpace) Column(Modifier.clearAndSetSemantics {}) {
                Text("", style = MaterialTheme.typography.headlineSmall, minLines = 2)
                Text("", style = MaterialTheme.typography.titleMedium)
            }
            Column(Modifier.fillMaxWidth()) {
                Text(
                    shown?.title.orEmpty(),
                    style = MaterialTheme.typography.headlineSmall,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    shown?.byline().orEmpty(),
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

/**
 * The seekbar and the buttons under it and, while casting, the cast volume ([CastVolume]) under
 * those, if there's room for it. It's left out sooner than it squeezes the buttons, as on a
 * window short for its width, where the volume buttons and the cast picker still have it.
 */
@Composable
private fun PlayerControls(playback: Playback, track: Track, onAddToPlaylist: (() -> Unit)?, modifier: Modifier = Modifier) {
    val cast = playback.cast
    UnlessShort(
        main = { TransportControls(playback, track, onAddToPlaylist) },
        extra = { if (cast.casting) CastVolume(cast) },
        modifier = modifier,
    )
}

/**
 * [main] and under it, [ControlsGap] away and centred, [extra], if what [main] leaves of the
 * height has room for it; if not, [extra] isn't placed, and takes no height.
 */
@Composable
private fun UnlessShort(main: @Composable () -> Unit, extra: @Composable () -> Unit, modifier: Modifier) {
    Layout(listOf(main, extra), modifier) { (mainItems, extraItems), constraints ->
        val mainPlaced = mainItems.single().measure(constraints.copy(minHeight = 0))
        val gap = ControlsGap.roundToPx()
        val extraPlaced = extraItems.singleOrNull()
            ?.measure(constraints.copy(minWidth = 0, minHeight = 0, maxHeight = Constraints.Infinity))
            ?.takeIf { !constraints.hasBoundedHeight || mainPlaced.height + gap + it.height <= constraints.maxHeight }
        val width = constraints.constrainWidth(maxOf(mainPlaced.width, extraPlaced?.width ?: 0))
        val height = constraints.constrainHeight(mainPlaced.height + (extraPlaced?.let { gap + it.height } ?: 0))
        layout(width, height) {
            mainPlaced.place(0, 0)
            if (extraPlaced != null) extraPlaced.place((width - extraPlaced.width) / 2, mainPlaced.height + gap)
        }
    }
}

@Composable
private fun TransportControls(playback: Playback, track: Track, onAddToPlaylist: (() -> Unit)?) {
    Column(verticalArrangement = Arrangement.spacedBy(ControlsGap)) {
        SeekbarWithTimes(
            positionMs = { playback.positionMs },
            durationMs = track.durationMs,
            onSeek = playback::seekTo,
        )
        // Add to playlist and shuffle at the ends, the playing controls between, centred.
        Row(
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth().widthIn(max = MaxArtSize).align(Alignment.CenterHorizontally),
        ) {
            IconButton(onClick = { onAddToPlaylist?.invoke() }, enabled = onAddToPlaylist != null, modifier = Modifier.offset(x = (-4).dp)) {
                Icon(Icons.PlaylistAdd, contentDescription = stringResource(R.string.add_to_playlist))
            }
            IconButton(onClick = playback::previous, modifier = Modifier.size(56.dp)) {
                Icon(Icons.SkipPrevious, contentDescription = stringResource(R.string.previous_track), modifier = Modifier.size(32.dp))
            }
            // Smaller, but still round, when the height is short (landscape with large text).
            FilledIconButton(
                onClick = playback::toggle,
                modifier = Modifier.sizeIn(maxWidth = 80.dp, maxHeight = 80.dp).aspectRatio(1f, matchHeightConstraintsFirst = true),
            ) {
                if (playback.isPlaying) Icon(Icons.Pause, contentDescription = stringResource(R.string.pause), modifier = Modifier.size(40.dp))
                else Icon(Icons.Play, contentDescription = stringResource(R.string.play), modifier = Modifier.size(40.dp))
            }
            IconButton(onClick = playback::next, enabled = playback.hasNext, modifier = Modifier.size(56.dp)) {
                Icon(Icons.SkipNext, contentDescription = stringResource(R.string.next_track), modifier = Modifier.size(32.dp))
            }
            IconToggleButton(
                checked = playback.shuffle,
                onCheckedChange = playback::setShuffle,
                // A station has its own order; shuffle is back, as it was, with the next queue.
                enabled = !playback.station,
                colors = IconButtonDefaults.iconToggleButtonColors(
                    checkedContainerColor = MaterialTheme.colorScheme.secondaryContainer,
                    checkedContentColor = MaterialTheme.colorScheme.onSecondaryContainer,
                ),
                modifier = Modifier.offset(x = 4.dp),
            ) {
                Icon(Icons.Shuffle, contentDescription = stringResource(R.string.shuffle))
            }
        }
    }
}
