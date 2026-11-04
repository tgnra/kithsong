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
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.onLongClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.dp
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
 * beneath. Tapping it calls [onOpen] and a long press [onMenu]; swiping it left or right skips
 * a track (see [TrackSwipeState]), which accessibility services get as actions.
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
 * The full player, over a background coloured from the art: large art, then title and byline, seekbar, and play/pause
 * between previous and next, with Add to playlist ([onAddToPlaylist], disabled if null) and
 * the shuffle toggle at the ends. [onCollapse] slides it away; it is also dismissed by swiping
 * down (see [DismissibleScreen]). Swiping left or right skips a track, moving the art and title
 * (see [TrackSwipeState]). [onMenu], if given, opens the track's menu from a button across from
 * the collapse button, and from a long press on the art.
 */
@Composable
fun FullPlayer(
    playback: Playback,
    track: Track,
    onCollapse: () -> Unit,
    onAddToPlaylist: (() -> Unit)? = null,
    onMenu: (() -> Unit)? = null,
) {
    val swipe = rememberTrackSwipe(playback)
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize().trackSwipe(swipe)) {
        ArtBackdrop(rememberArtColors(track.art), Modifier.fillMaxSize())
        BoxWithConstraints(Modifier.windowInsetsPadding(WindowInsets.safeDrawing)) {
            val wide = maxWidth > maxHeight
            Column(Modifier.fillMaxSize().padding(horizontal = 24.dp)) {
                // Nudged so the icons, not their touch targets, line up with the art.
                Row(Modifier.fillMaxWidth().padding(top = 4.dp)) {
                    IconButton(onClick = onCollapse, modifier = Modifier.offset(x = (-12).dp)) {
                        Icon(Icons.ExpandMore, contentDescription = stringResource(R.string.close_player))
                    }
                    Spacer(Modifier.weight(1f))
                    if (onMenu != null) IconButton(onClick = onMenu, modifier = Modifier.offset(x = 12.dp)) {
                        Icon(Icons.MoreVert, contentDescription = stringResource(R.string.track_menu))
                    }
                }
                if (wide) {
                    Row(Modifier.fillMaxSize().padding(bottom = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                        PlayerArt(swipe, onMenu, Modifier.fillMaxHeight())
                        Spacer(Modifier.size(32.dp))
                        Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(ControlsGap)) {
                            PlayerTitle(swipe)
                            PlayerControls(playback, track, onAddToPlaylist)
                        }
                    }
                } else {
                    TallPlayerLayout(
                        art = { PlayerArt(swipe, onMenu, Modifier) },
                        title = { PlayerTitle(swipe) },
                        controls = { PlayerControls(playback, track, onAddToPlaylist) },
                        modifier = Modifier.weight(1f),
                    )
                }
            }
        }
    }
}

/**
 * The full player's parts down a window taller than it's wide. The art is as big as it can be
 * in the height the rest leaves. Height left over from that goes above the art and above the
 * seekbar, and past their limits, around the lot, so it's never all at the bottom.
 *
 * What's left over is worked out as if the title took two lines, and the title sits under the
 * art and the controls at the bottom, so moving between a short title and a long one only
 * changes the gap between them, not where the art or the buttons are.
 */
@Composable
private fun TallPlayerLayout(
    art: @Composable () -> Unit,
    title: @Composable () -> Unit,
    controls: @Composable () -> Unit,
    modifier: Modifier,
) {
    val titleLine = MaterialTheme.typography.headlineSmall.lineHeight
    val bylineLine = MaterialTheme.typography.titleMedium.lineHeight
    Layout(listOf(art, title, controls), modifier) { (artItem, titleItem, controlsItem), constraints ->
        val width = constraints.maxWidth
        val height = constraints.maxHeight
        val artGap = ArtGap.roundToPx()
        val bottom = BottomGap.roundToPx()
        val fixed = artGap + ControlsGap.roundToPx() + bottom
        val titlePlaced = titleItem.single().measure(Constraints(width, width, 0, height))
        // The controls come first, as they shrink only when there's no other way.
        val controlsPlaced = controlsItem.single()
            .measure(Constraints(width, width, 0, (height - titlePlaced.height - fixed).coerceAtLeast(0)))
        val room = height - titlePlaced.height - controlsPlaced.height - fixed
        val side = minOf(width, MaxArtSize.roundToPx(), room).coerceAtLeast(0)
        val artPlaced = artItem.single().measure(Constraints.fixed(side, side))

        val longTitle = titleLine.roundToPx() * 2 + bylineLine.roundToPx()
        val spare = (room - side - (longTitle - titlePlaced.height).coerceAtLeast(0)).coerceAtLeast(0)
        val aboveArt = minOf(spare * 3 / 5, MaxGapAboveArt.roundToPx())
        val aboveSeekbar = minOf(spare * 2 / 5, MaxGapAboveSeekbar.roundToPx())
        val around = (spare - aboveArt - aboveSeekbar) / 2
        layout(width, height) {
            val artTop = around + aboveArt
            artPlaced.place((width - side) / 2, artTop)
            titlePlaced.place(0, artTop + side + artGap)
            controlsPlaced.place(0, height - around - bottom - controlsPlaced.height)
        }
    }
}

/** The largest the full player's art gets, and the width of the buttons under it. */
private val MaxArtSize = 480.dp

/** Under the art, under the controls, and between the title and the controls, at the least. */
private val ArtGap = 24.dp
private val BottomGap = 24.dp
private val ControlsGap = 16.dp

/** The most of what a tall window leaves over that goes above the art and above the seekbar. */
private val MaxGapAboveArt = 96.dp
private val MaxGapAboveSeekbar = 64.dp

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

@Composable
private fun PlayerTitle(swipe: TrackSwipeState) {
    TrackSlides(swipe) { shown ->
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

@Composable
private fun PlayerControls(playback: Playback, track: Track, onAddToPlaylist: (() -> Unit)?) {
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
