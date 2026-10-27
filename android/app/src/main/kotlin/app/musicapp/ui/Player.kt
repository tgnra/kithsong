package app.musicapp.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
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
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import app.musicapp.R
import app.musicapp.data.Track
import app.musicapp.playback.Playback
import app.musicapp.ui.art.AlbumArt

/** "Artist - Album", under a track's title. */
@Composable
private fun Track.byline() = stringResource(R.string.track_byline, artist, album)

/**
 * The floating player above the content: art, title and byline, play/pause, and a seekbar
 * beneath. Tapping it calls [onOpen].
 */
@Composable
fun MiniPlayer(playback: Playback, track: Track, onOpen: () -> Unit, modifier: Modifier = Modifier) {
    Surface(
        shape = RoundedCornerShape(20.dp),
        color = MaterialTheme.colorScheme.surfaceContainerHigh,
        tonalElevation = 3.dp,
        shadowElevation = 6.dp,
        modifier = modifier,
    ) {
        Column(
            Modifier
                .clickable(onClickLabel = stringResource(R.string.open_player), onClick = onOpen)
                .padding(start = 10.dp, end = 4.dp, top = 10.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                AlbumArt(track.art, contentDescription = null, shape = RoundedCornerShape(8.dp), modifier = Modifier.size(48.dp))
                Column(Modifier.weight(1f).padding(horizontal = 12.dp)) {
                    Text(track.title, style = MaterialTheme.typography.titleSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    Text(
                        track.byline(),
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
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

@Composable
private fun PlayPause(playback: Playback) {
    IconButton(onClick = playback::toggle) {
        if (playback.isPlaying) Icon(Icons.Pause, contentDescription = stringResource(R.string.pause))
        else Icon(Icons.Play, contentDescription = stringResource(R.string.play))
    }
}

/**
 * The full player: large art, title and byline, seekbar, and play/pause between previous and
 * next. [onCollapse] slides it away; it is also dismissed by swiping down (see
 * [DismissibleScreen]).
 */
@Composable
fun FullPlayer(playback: Playback, track: Track, onCollapse: () -> Unit) {
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        BoxWithConstraints(Modifier.windowInsetsPadding(WindowInsets.safeDrawing)) {
            val wide = maxWidth > maxHeight
            Column(Modifier.fillMaxSize().padding(horizontal = 24.dp)) {
                // Nudged so the icon, not its touch target, lines up with the art.
                IconButton(onClick = onCollapse, modifier = Modifier.padding(top = 4.dp).offset(x = (-12).dp)) {
                    Icon(Icons.ExpandMore, contentDescription = stringResource(R.string.close_player))
                }
                if (wide) {
                    Row(Modifier.fillMaxSize().padding(bottom = 16.dp), verticalAlignment = Alignment.CenterVertically) {
                        PlayerArt(track, Modifier.fillMaxHeight())
                        Spacer(Modifier.size(32.dp))
                        PlayerControls(playback, track, Modifier.weight(1f))
                    }
                } else {
                    Spacer(Modifier.weight(0.5f))
                    PlayerArt(track, Modifier.fillMaxWidth().align(Alignment.CenterHorizontally))
                    Spacer(Modifier.weight(0.5f))
                    PlayerControls(playback, track, Modifier.padding(bottom = 32.dp))
                }
            }
        }
    }
}

@Composable
private fun PlayerArt(track: Track, modifier: Modifier) {
    AlbumArt(
        track.art,
        contentDescription = stringResource(R.string.cover_of, track.album),
        shape = RoundedCornerShape(24.dp),
        modifier = modifier.widthIn(max = 480.dp).aspectRatio(1f),
    )
}

@Composable
private fun PlayerControls(playback: Playback, track: Track, modifier: Modifier) {
    Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        Column {
            Text(
                track.title,
                style = MaterialTheme.typography.headlineSmall,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                track.byline(),
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        SeekbarWithTimes(
            positionMs = { playback.positionMs },
            durationMs = track.durationMs,
            onSeek = playback::seekTo,
        )
        Row(
            horizontalArrangement = Arrangement.spacedBy(24.dp),
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.align(Alignment.CenterHorizontally),
        ) {
            IconButton(onClick = playback::previous, modifier = Modifier.size(56.dp)) {
                Icon(Icons.SkipPrevious, contentDescription = stringResource(R.string.previous_track), modifier = Modifier.size(32.dp))
            }
            FilledIconButton(onClick = playback::toggle, modifier = Modifier.size(80.dp)) {
                if (playback.isPlaying) Icon(Icons.Pause, contentDescription = stringResource(R.string.pause), modifier = Modifier.size(40.dp))
                else Icon(Icons.Play, contentDescription = stringResource(R.string.play), modifier = Modifier.size(40.dp))
            }
            IconButton(onClick = playback::next, enabled = playback.hasNext, modifier = Modifier.size(56.dp)) {
                Icon(Icons.SkipNext, contentDescription = stringResource(R.string.next_track), modifier = Modifier.size(32.dp))
            }
        }
    }
}
