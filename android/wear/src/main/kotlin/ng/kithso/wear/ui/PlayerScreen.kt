package ng.kithso.wear.ui

import android.view.ViewConfiguration
import androidx.compose.foundation.focusable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.rotary.onRotaryScrollEvent
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.wear.compose.foundation.requestFocusOnHierarchyActive
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.CircularProgressIndicator
import androidx.wear.compose.material3.FilledIconButton
import androidx.wear.compose.material3.Icon
import androidx.wear.compose.material3.IconButton
import androidx.wear.compose.material3.IconButtonDefaults
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.ScreenScaffold
import androidx.wear.compose.material3.Text
import kotlinx.coroutines.delay
import ng.kithso.wear.R

/**
 * The player: the track, its progress as an arc round the edge of the screen with the times
 * under the buttons, and the three buttons. [title] is null when nothing is queued.
 * [positionMs] is read only where it's shown, so its ticking redraws the arc and the times and
 * nothing else. The crown seeks: each turn moves a target position, shown at once and sent to
 * the player ([onSeek]) when the turning pauses, so a long turn is one seek.
 */
@Composable
fun PlayerScreen(
    title: String?,
    artist: String?,
    isPlaying: Boolean,
    hasPrevious: Boolean,
    hasNext: Boolean,
    positionMs: () -> Long,
    durationMs: Long,
    onPrevious: () -> Unit,
    onPlayPause: () -> Unit,
    onNext: () -> Unit,
    onSeek: (Long) -> Unit,
) {
    var seekTarget by remember { mutableStateOf<Long?>(null) }
    LaunchedEffect(seekTarget) {
        val target = seekTarget ?: return@LaunchedEffect
        delay(SEEK_SETTLE_MS)
        onSeek(target)
        seekTarget = null
    }
    // One detent of the crown scrolls this many pixels, by the system's reckoning.
    val context = LocalContext.current
    val detentPx = remember(context) { ViewConfiguration.get(context).scaledVerticalScrollFactor }
    val shown = { seekTarget ?: positionMs() }
    ScreenScaffold(
        modifier = Modifier
            .onRotaryScrollEvent { event ->
                if (title == null || durationMs <= 0) return@onRotaryScrollEvent false
                val detents = event.verticalScrollPixels / detentPx
                seekTarget = (shown() + (detents * SEEK_MS_PER_DETENT).toLong()).coerceIn(0, durationMs)
                true
            }
            .requestFocusOnHierarchyActive()
            .focusable(),
    ) {
        Box(Modifier.fillMaxSize()) {
            if (title != null && durationMs > 0) {
                CircularProgressIndicator(
                    progress = { (shown().toFloat() / durationMs).coerceIn(0f, 1f) },
                    modifier = Modifier.fillMaxSize().padding(1.dp),
                    // Round the edge from the top, leaving the top itself to the time.
                    startAngle = 300f,
                    endAngle = 240f,
                    strokeWidth = 3.dp,
                )
            }
            Column(
                modifier = Modifier.fillMaxSize(),
                verticalArrangement = Arrangement.Center,
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Text(
                    text = title ?: stringResource(R.string.nothing_playing),
                    // Round screens clip the corners; keep text inside the inscribed square.
                    modifier = Modifier.padding(horizontal = 28.dp),
                    style = MaterialTheme.typography.titleMedium,
                    textAlign = TextAlign.Center,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    text = artist.orEmpty(),
                    modifier = Modifier.padding(horizontal = 28.dp),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Spacer(Modifier.height(8.dp))
                Row(
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    IconButton(onClick = onPrevious, enabled = hasPrevious) {
                        Icon(Icons.SkipPrevious, contentDescription = stringResource(R.string.previous_track))
                    }
                    FilledIconButton(
                        onClick = onPlayPause,
                        enabled = title != null,
                        modifier = Modifier.size(IconButtonDefaults.LargeButtonSize),
                    ) {
                        Icon(
                            if (isPlaying) Icons.Pause else Icons.Play,
                            contentDescription = stringResource(if (isPlaying) R.string.pause else R.string.play),
                            modifier = Modifier.size(IconButtonDefaults.LargeIconSize),
                        )
                    }
                    IconButton(onClick = onNext, enabled = hasNext) {
                        Icon(Icons.SkipNext, contentDescription = stringResource(R.string.next_track))
                    }
                }
                if (title != null) {
                    Spacer(Modifier.height(4.dp))
                    Times(shown, durationMs)
                }
            }
        }
    }
}

/** The time played of the track's length; a composable of its own, so only it ticks. */
@Composable
private fun Times(positionMs: () -> Long, durationMs: Long) {
    val text = stringResource(R.string.watch_times, formatTime(positionMs()), formatTime(durationMs))
    val name = stringResource(R.string.seek)
    Text(
        text,
        modifier = Modifier.semantics {
            contentDescription = name
            stateDescription = text
        },
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
    )
}

/** [ms] as m:ss, or h:mm:ss from an hour. */
internal fun formatTime(ms: Long): String {
    val seconds = (ms / 1000).coerceAtLeast(0)
    val h = seconds / 3600
    val m = seconds / 60 % 60
    val s = seconds % 60
    return if (h > 0) "%d:%02d:%02d".format(h, m, s) else "%d:%02d".format(m, s)
}

// Turning the crown one detent seeks this far; the seek is sent once no turn has come for
// SEEK_SETTLE_MS.
private const val SEEK_MS_PER_DETENT = 5_000L
private const val SEEK_SETTLE_MS = 300L

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun PlayerScreenPreview() {
    MaterialTheme {
        AppScaffold {
            PlayerScreen(
                "Salt and Glass", "The Harbour Lamps", isPlaying = true, hasPrevious = true, hasNext = true,
                positionMs = { 83_000 }, durationMs = 245_000,
                onPrevious = {}, onPlayPause = {}, onNext = {}, onSeek = {},
            )
        }
    }
}
