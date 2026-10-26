package app.musicapp.wear.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.FilledIconButton
import androidx.wear.compose.material3.Icon
import androidx.wear.compose.material3.IconButton
import androidx.wear.compose.material3.IconButtonDefaults
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.ScreenScaffold
import androidx.wear.compose.material3.Text

/** The player controls; [title] is null when nothing is queued. */
@Composable
fun PlayerScreen(
    title: String?,
    artist: String?,
    isPlaying: Boolean,
    hasPrevious: Boolean,
    hasNext: Boolean,
    onPrevious: () -> Unit,
    onPlayPause: () -> Unit,
    onNext: () -> Unit,
) {
    ScreenScaffold {
        Column(
            modifier = Modifier.fillMaxSize(),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                text = title ?: "Nothing playing",
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
            Spacer(Modifier.height(12.dp))
            Row(
                horizontalArrangement = Arrangement.spacedBy(4.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                IconButton(onClick = onPrevious, enabled = hasPrevious) {
                    Icon(Icons.SkipPrevious, contentDescription = "Previous track")
                }
                FilledIconButton(
                    onClick = onPlayPause,
                    enabled = title != null,
                    modifier = Modifier.size(IconButtonDefaults.LargeButtonSize),
                ) {
                    Icon(
                        if (isPlaying) Icons.Pause else Icons.Play,
                        contentDescription = if (isPlaying) "Pause" else "Play",
                        modifier = Modifier.size(IconButtonDefaults.LargeIconSize),
                    )
                }
                IconButton(onClick = onNext, enabled = hasNext) {
                    Icon(Icons.SkipNext, contentDescription = "Next track")
                }
            }
        }
    }
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun PlayerScreenPreview() {
    MaterialTheme {
        AppScaffold {
            PlayerScreen(
                "Salt and Glass", "The Harbour Lamps", isPlaying = true, hasPrevious = true,
                hasNext = true, onPrevious = {}, onPlayPause = {}, onNext = {},
            )
        }
    }
}
