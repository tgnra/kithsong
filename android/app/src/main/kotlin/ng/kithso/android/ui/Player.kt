package ng.kithso.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
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
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.onLongClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.min
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
 * beneath, which a dense window leaves out, with smaller art. Tapping it calls [onOpen] and a long press [onMenu]; swiping it left or right skips
 * a track (see [TrackSwipeState]), which accessibility services get as actions.
 */
@Composable
fun MiniPlayer(playback: Playback, track: Track, onOpen: () -> Unit, onMenu: () -> Unit, modifier: Modifier = Modifier) {
    val dense = LocalWindowLayout.current.dense
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
                .padding(start = if (dense) 6.dp else 10.dp, end = 4.dp, top = if (dense) 0.dp else 10.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                TrackSlides(swipe, Modifier.weight(1f).clipToBounds()) { shown ->
                    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        TrackArt(shown, contentDescription = null, shape = RoundedCornerShape(8.dp), modifier = Modifier.size(if (dense) 40.dp else 48.dp))
                        Column(Modifier.weight(1f).padding(horizontal = if (dense) 8.dp else 12.dp)) {
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
            if (!dense) Seekbar(
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
 * how far. A window too short to spare the height has that chevron in the middle of the top row,
 * as has any dense one.
 * Only the player's own view swipes between tracks, and the background stays put under both.
 * [queueBackEnabled]: system back is the queue's while it's open.
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
    QueueBackHandler(slide, queueBackEnabled)
    Surface(
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxSize().onSizeChanged { slide.page = it.height.toFloat() },
    ) {
        ArtBackdrop(rememberArtColors(track.art), Modifier.fillMaxSize())
        BoxWithConstraints(
            Modifier
                .fillMaxSize()
                .graphicsLayer { translationY = -slide.progress * slide.page }
                .coveredIf(slide.open)
                .queueDrag(slide, swipe)
                .trackSwipe(swipe)
                .windowInsetsPadding(WindowInsets.safeDrawing.inPane()),
        ) {
            val dense = LocalWindowLayout.current.dense
            val sidePadding = playerSidePadding(dense)
            val fontScale = LocalDensity.current.fontScale
            val artToControls = playerArtToControls(dense)
            val artMax = playerArtMaxBeside(maxWidth, dense)
            val wide = playerIsWide(maxWidth, maxHeight, dense, fontScale)
            // Below about this, two lines and the controls don't fit, so a short title gives its space back.
            val titleSpace = !dense && maxHeight >= 256.dp + 56.dp * fontScale
            val playSize = if (maxWidth - sidePadding * 2 >= MinDenseControlsWidth) 64.dp else 56.dp
            val gap = if (dense) DenseControlsGap else ControlsGap
            Column(Modifier.fillMaxSize().padding(horizontal = sidePadding)) {
                // Nudged so the icons, not their touch targets, line up with the art.
                Row(Modifier.fillMaxWidth().padding(top = 4.dp)) {
                    IconButton(onClick = onCollapse, modifier = Modifier.offset(x = (-12).dp)) {
                        Icon(Icons.ExpandMore, contentDescription = stringResource(R.string.close_player))
                    }
                    Spacer(Modifier.weight(1f))
                    if (wide || dense) {
                        QueueChevron(slide)
                        Spacer(Modifier.weight(1f))
                    }
                    if (onMenu != null) IconButton(onClick = onMenu, modifier = Modifier.offset(x = 12.dp)) {
                        Icon(Icons.MoreVert, contentDescription = stringResource(R.string.track_menu))
                    }
                }
                if (wide) {
                    Row(Modifier.fillMaxSize().padding(bottom = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                        PlayerArt(swipe, onMenu, Modifier.heightIn(max = artMax.coerceAtLeast(0.dp)).fillMaxHeight())
                        Spacer(Modifier.size(artToControls))
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(gap)) {
                            PlayerTitle(swipe, titleSpace, Alignment.CenterStart)
                            // Moved, not closed up, so this centred column keeps its height and the title stays put.
                            PlayerControls(playback, track, onAddToPlaylist, playSize, Modifier.offset(y = -gap))
                        }
                    }
                } else {
                    TallPlayerLayout(
                        art = { PlayerArt(swipe, onMenu, Modifier) },
                        title = { PlayerTitle(swipe, titleSpace, Alignment.TopStart) },
                        controls = { PlayerControls(playback, track, onAddToPlaylist, playSize) },
                        chevron = { if (dense) Spacer(Modifier) else QueueChevron(slide) },
                        dense = dense,
                        modifier = Modifier.weight(1f),
                    )
                }
            }
        }
        if (slide.shown) {
            QueuePage(
                playback,
                slide,
                Modifier
                    .fillMaxSize()
                    .graphicsLayer { translationY = (1f - slide.progress) * slide.page }
                    .queueDrag(slide)
                    .windowInsetsPadding(WindowInsets.safeDrawing.inPane()),
            )
        }
    }
}

/** The full player's padding down each side. */
private fun playerSidePadding(dense: Boolean) = if (dense) 16.dp else 24.dp

/** What's between the art and the controls beside it. */
private fun playerArtToControls(dense: Boolean) = if (dense) 20.dp else 32.dp

/** The most the art can be beside the controls in a window [width] across, leaving them their width. */
internal fun playerArtMaxBeside(width: Dp, dense: Boolean): Dp =
    width - playerSidePadding(dense) * 2 - playerArtToControls(dense) - (if (dense) MinDenseControlsWidth else MinControlsWidth)

/**
 * Whether the art goes beside the controls in a window [width] by [height]: if it's wider than
 * it's tall, and the art's as big there as above them. In a landscape phone's half pane the art
 * gives way so much to the controls' width that it can end up better above them.
 */
internal fun playerIsWide(width: Dp, height: Dp, dense: Boolean, fontScale: Float): Boolean {
    val beside = min(playerArtMaxBeside(width, dense), height - TopRowHeight - 16.dp)
    val above = height - TopRowHeight - (if (dense) 176.dp + 24.dp * fontScale else 256.dp + 56.dp * fontScale)
    return width > height && beside >= above
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
 * stays at the foot, in the room below the controls. A [dense] window has less between them.
 */
@Composable
private fun TallPlayerLayout(
    art: @Composable () -> Unit,
    title: @Composable () -> Unit,
    controls: @Composable () -> Unit,
    chevron: @Composable () -> Unit,
    dense: Boolean,
    modifier: Modifier,
) {
    Layout(listOf(art, title, controls, chevron), modifier) { (artItem, titleItem, controlsItem, chevronItem), constraints ->
        val width = constraints.maxWidth
        val height = constraints.maxHeight
        val artGap = (if (dense) DenseArtGap else ArtGap).roundToPx()
        val controlsGap = (if (dense) DenseControlsGap else ControlsGap).roundToPx()
        val chevronPlaced = chevronItem.single().measure(Constraints())
        val bottom = maxOf((if (dense) DenseBottomGap else BottomGap).roundToPx(), chevronPlaced.height - controlsGap)
        // ControlsGap isn't placed, it's part of the room below the controls, which sizes the art.
        val fixed = artGap + controlsGap + bottom
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

/** The largest the full player's art gets, and the width of the buttons under it. */
private val MaxArtSize = 480.dp

/**
 * Under the art, under the controls at the least, and between the seekbar and the buttons. The
 * seekbar's touch target leaves 12dp above its thumb, which is the gap under the title.
 */
private val ArtGap = 24.dp
private val BottomGap = 24.dp
private val ControlsGap = 16.dp

/** The same in a dense window. */
private val DenseArtGap = 12.dp
private val DenseBottomGap = 8.dp
private val DenseControlsGap = 2.dp

/** The row of buttons at the top, with what's above it. */
private val TopRowHeight = 52.dp

/** The width of the controls' five buttons with a little to spare, and the same in a dense window. */
private val MinControlsWidth = 296.dp
private val MinDenseControlsWidth = 264.dp

/** The most of what a tall window leaves over that goes above the art and below the controls. */
private val MaxGapAboveArt = 96.dp
private val MaxGapBelowControls = 64.dp

/**
 * [onMenu], if given, is a long press on the art, with the buzz a long press has elsewhere.
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

/** The title and byline, a line each and smaller in a dense window. With [twoLineSpace], always the height of two, with a shorter one at [alignment]. */
@Composable
private fun PlayerTitle(swipe: TrackSwipeState, twoLineSpace: Boolean, alignment: Alignment) {
    val dense = LocalWindowLayout.current.dense
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
                    style = if (dense) MaterialTheme.typography.titleLarge else MaterialTheme.typography.headlineSmall,
                    maxLines = if (dense) 1 else 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    shown?.byline().orEmpty(),
                    style = if (dense) MaterialTheme.typography.titleSmall else MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
    }
}

@Composable
private fun PlayerControls(
    playback: Playback,
    track: Track,
    onAddToPlaylist: (() -> Unit)?,
    densePlaySize: Dp,
    modifier: Modifier = Modifier,
) {
    val dense = LocalWindowLayout.current.dense
    val skipSize = if (dense) 48.dp else 56.dp
    val skipIconSize = if (dense) 28.dp else 32.dp
    Column(modifier, verticalArrangement = Arrangement.spacedBy(if (dense) DenseControlsGap else ControlsGap)) {
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
            IconButton(onClick = playback::previous, modifier = Modifier.size(skipSize)) {
                Icon(Icons.SkipPrevious, contentDescription = stringResource(R.string.previous_track), modifier = Modifier.size(skipIconSize))
            }
            // Smaller, but still round, when the height is short (landscape with large text). A dense
            // window has its size set, so that nothing squeezes it.
            val playIconSize = if (dense) 32.dp else 40.dp
            FilledIconButton(
                onClick = playback::toggle,
                modifier = if (dense) Modifier.size(densePlaySize) else {
                    Modifier.sizeIn(maxWidth = 80.dp, maxHeight = 80.dp).aspectRatio(1f, matchHeightConstraintsFirst = true)
                },
            ) {
                if (playback.isPlaying) Icon(Icons.Pause, contentDescription = stringResource(R.string.pause), modifier = Modifier.size(playIconSize))
                else Icon(Icons.Play, contentDescription = stringResource(R.string.play), modifier = Modifier.size(playIconSize))
            }
            IconButton(onClick = playback::next, enabled = playback.hasNext, modifier = Modifier.size(skipSize)) {
                Icon(Icons.SkipNext, contentDescription = stringResource(R.string.next_track), modifier = Modifier.size(skipIconSize))
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
