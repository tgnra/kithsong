package ng.kithso.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.CircleShape
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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.Track
import ng.kithso.android.playback.Playback
import ng.kithso.android.ui.art.AlbumArt
import ng.kithso.android.ui.theme.ArtTheme

/** The top bar's height under the status bar. */
internal val TopBarHeight = 64.dp

/**
 * The tablet layout's top bar, as the desktops' header bar: previous, play and next, then the
 * now-playing strip, in the playing track's colours as the mini player is. The strip has the
 * cover in a rounded square, the title with the artist dimmed after it, and the seekbar with
 * the time so far on its left and the time left on its right, then the shuffle toggle and the
 * queue's button ([onQueue]). Tapping the cover opens the full player ([onOpenPlayer]), and a
 * long press the track's menu ([onMenu]), as does a right-click on the cover or the title; there's no mini player in this layout. With nothing
 * playing it says so, and the controls are off.
 */
@Composable
internal fun TopBar(
    playback: Playback,
    onOpenPlayer: () -> Unit,
    onQueue: () -> Unit,
    onMenu: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val track = playback.track
    if (track != null) ArtTheme(track.art) { TopBarContent(playback, track, onOpenPlayer, onQueue, onMenu, modifier) }
    else TopBarContent(playback, null, onOpenPlayer, onQueue, onMenu, modifier)
}

@Composable
private fun TopBarContent(
    playback: Playback,
    track: Track?,
    onOpenPlayer: () -> Unit,
    onQueue: () -> Unit,
    onMenu: () -> Unit,
    modifier: Modifier,
) {
    val playing = track != null
    Surface(color = MaterialTheme.colorScheme.surfaceContainer, modifier = modifier.fillMaxWidth()) {
        Row(
            Modifier
                .windowInsetsPadding(WindowInsets.statusBars.union(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)))
                .height(TopBarHeight)
                .padding(horizontal = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            // Each has a ring for the keyboard's focus, which Material only tints.
            IconButton(onClick = playback::previous, enabled = playing, modifier = Modifier.focusRing(CircleShape)) {
                Icon(Icons.SkipPrevious, contentDescription = stringResource(R.string.previous_track))
            }
            FilledIconButton(onClick = playback::toggle, enabled = playing, modifier = Modifier.focusRing(CircleShape)) {
                if (playback.isPlaying) Icon(Icons.Pause, contentDescription = stringResource(R.string.pause))
                else Icon(Icons.Play, contentDescription = stringResource(R.string.play))
            }
            IconButton(onClick = playback::next, enabled = playback.hasNext, modifier = Modifier.focusRing(CircleShape)) {
                Icon(Icons.SkipNext, contentDescription = stringResource(R.string.next_track))
            }
            Spacer(Modifier.width(16.dp))
            NowPlayingStrip(playback, track, onOpenPlayer, onMenu, Modifier.weight(1f).widthIn(max = MaxStripWidth))
            Spacer(Modifier.width(8.dp))
            IconToggleButton(
                checked = playback.shuffle,
                onCheckedChange = playback::setShuffle,
                // A station has its own order, as on the full player.
                enabled = playing && !playback.station,
                modifier = Modifier.focusRing(CircleShape),
                colors = IconButtonDefaults.iconToggleButtonColors(
                    checkedContainerColor = MaterialTheme.colorScheme.secondaryContainer,
                    checkedContentColor = MaterialTheme.colorScheme.onSecondaryContainer,
                ),
            ) {
                Icon(Icons.Shuffle, contentDescription = stringResource(R.string.shuffle))
            }
            IconButton(onClick = onQueue, enabled = playing, modifier = Modifier.focusRing(CircleShape)) {
                Icon(Icons.QueueMusic, contentDescription = stringResource(R.string.queue))
            }
        }
    }
}

/** The strip's widest, as on a desktop's header bar: past this the seekbar is just longer to drag. */
private val MaxStripWidth = 720.dp

/** The cover, the title and artist, and the seekbar between the times. */
@Composable
private fun NowPlayingStrip(playback: Playback, track: Track?, onOpenPlayer: () -> Unit, onMenu: () -> Unit, modifier: Modifier) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        val shape = RoundedCornerShape(8.dp)
        val art = Modifier.size(StripArtSize)
        if (track != null) {
            AlbumArt(
                track.art,
                contentDescription = stringResource(R.string.open_player),
                shape = shape,
                // The menu key opens the menu too, as on a tile.
                modifier = art.clip(shape).listItemKeys(shape, onMenu).menuClickable(
                    onMenu,
                    onOpenPlayer,
                    onClickLabel = stringResource(R.string.open_player),
                    onMenuLabel = stringResource(R.string.track_menu),
                ),
            )
        } else {
            Box(art.clip(shape).background(MaterialTheme.colorScheme.surfaceVariant))
        }
        Column(Modifier.weight(1f).padding(start = 12.dp)) {
            val dim = MaterialTheme.colorScheme.onSurfaceVariant
            Text(
                if (track == null) {
                    buildAnnotatedString { withStyle(SpanStyle(color = dim)) { append(stringResource(R.string.nothing_playing)) } }
                } else {
                    val artist = if (track.artist.isEmpty()) "" else " " + stringResource(R.string.now_playing_artist, track.artist)
                    buildAnnotatedString {
                        append(track.title)
                        withStyle(SpanStyle(color = dim)) { append(artist) }
                    }
                },
                style = MaterialTheme.typography.titleSmall,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                // A right-click on the title opens the menu too, as on the cover.
                modifier = if (track != null) Modifier.secondaryClick(onMenu) else Modifier,
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                val duration = track?.durationMs ?: 0L
                // The seekbar says where it is for TalkBack; the times would say it again.
                StripTime(Modifier.clearAndSetSemantics {}, TimeWidth) { formatTime(if (track == null) 0 else playback.positionMs) }
                Seekbar(
                    positionMs = { if (track == null) 0 else playback.positionMs },
                    durationMs = duration,
                    onSeek = playback::seekTo,
                    modifier = Modifier.weight(1f).height(24.dp).padding(horizontal = 8.dp),
                    trackHeight = 4.dp,
                    thumbHeight = 14.dp,
                    enabled = track != null,
                )
                // Whole seconds of each, as on the desktops, so the two add up to the length.
                StripTime(Modifier.clearAndSetSemantics {}, TimeWidth) {
                    val position = if (track == null) 0 else playback.positionMs / 1000 * 1000
                    "-" + formatTime((duration - position).coerceAtLeast(0))
                }
            }
        }
    }
}

/**
 * A time beside the seekbar, read in here so the position ticking recomposes only this. It's
 * at least [width] wide, so the seekbar doesn't shift as the digits change.
 */
@Composable
private fun StripTime(modifier: Modifier, width: Dp, text: () -> String) {
    Text(text(), style = MaterialTheme.typography.labelSmall, maxLines = 1, modifier = modifier.widthIn(min = width))
}

private val StripArtSize = 48.dp
private val TimeWidth = 36.dp
