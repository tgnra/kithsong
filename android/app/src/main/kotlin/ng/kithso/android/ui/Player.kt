package ng.kithso.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
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
 * beneath. Tapping it calls [onOpen]; swiping it left or right skips a track (see
 * [TrackSwipeState]), which accessibility services get as actions.
 */
@Composable
fun MiniPlayer(playback: Playback, track: Track, onOpen: () -> Unit, modifier: Modifier = Modifier) {
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
                .trackSwipe(swipe)
                .semantics {
                    customActions = listOfNotNull(
                        CustomAccessibilityAction(nextLabel) { playback.next(); true }.takeIf { playback.hasNext },
                        CustomAccessibilityAction(previousLabel) { playback.skipToPrevious(); true }.takeIf { playback.hasPrevious },
                    )
                }
                .clickable(onClickLabel = stringResource(R.string.open_player), onClick = onOpen)
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
 * The full player, over a background coloured from the art: large art at the top, then title and byline, seekbar, and play/pause
 * between previous and next, with Add to playlist ([onAddToPlaylist], disabled if null) and
 * the shuffle toggle at the ends. [onCollapse] slides it away; it is also dismissed by swiping
 * down (see [DismissibleScreen]). Swiping left or right skips a track, moving the art and title
 * (see [TrackSwipeState]). [onMenu], if given, opens the track's menu from a button across from
 * the collapse button.
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
                        PlayerArt(swipe, Modifier.fillMaxHeight())
                        Spacer(Modifier.size(32.dp))
                        PlayerControls(playback, swipe, track, onAddToPlaylist, Modifier.weight(1f))
                    }
                } else {
                    // The art at the top, as wide as it can be in the height the controls
                    // leave, and the controls just below it; what's left over is at the bottom.
                    PlayerArt(swipe, Modifier.weight(1f, fill = false).align(Alignment.CenterHorizontally))
                    PlayerControls(playback, swipe, track, onAddToPlaylist, Modifier.padding(top = 24.dp, bottom = 24.dp))
                }
            }
        }
    }
}

@Composable
private fun PlayerArt(swipe: TrackSwipeState, modifier: Modifier) {
    TrackSlides(swipe, modifier.widthIn(max = 480.dp).aspectRatio(1f)) { shown ->
        TrackArt(
            shown,
            contentDescription = shown?.let { stringResource(R.string.cover_of, it.album.ifEmpty { it.title }) },
            shape = RoundedCornerShape(24.dp),
            modifier = Modifier.fillMaxSize(),
        )
    }
}

@Composable
private fun PlayerControls(
    playback: Playback,
    swipe: TrackSwipeState,
    track: Track,
    onAddToPlaylist: (() -> Unit)?,
    modifier: Modifier,
) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
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
        SeekbarWithTimes(
            positionMs = { playback.positionMs },
            durationMs = track.durationMs,
            onSeek = playback::seekTo,
        )
        // Add to playlist and shuffle at the ends, the playing controls between, centred.
        Row(
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth().widthIn(max = 480.dp).align(Alignment.CenterHorizontally),
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
