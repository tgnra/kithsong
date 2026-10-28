package ng.kithso.android.ui

import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import ng.kithso.android.R

/**
 * A seekbar over [durationMs]. [positionMs] is read only in here, so the position ticking
 * recomposes the seekbar and nothing around it. While dragging it shows the drag position,
 * and seeks when let go.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun Seekbar(
    positionMs: () -> Long,
    durationMs: Long,
    onSeek: (Long) -> Unit,
    modifier: Modifier = Modifier,
    trackHeight: Dp = 4.dp,
    thumbHeight: Dp = 16.dp,
) {
    var dragging by remember { mutableStateOf<Float?>(null) }
    val duration = durationMs.coerceAtLeast(1).toFloat()
    val value = dragging ?: (positionMs() / duration).coerceIn(0f, 1f)
    val interaction = remember { MutableInteractionSource() }
    val description = stringResource(R.string.seek)
    val state = stringResource(R.string.seek_state, formatTime((value * duration).toLong()), formatTime(durationMs))
    Slider(
        value = value,
        onValueChange = { dragging = it },
        onValueChangeFinished = {
            dragging?.let { onSeek((it * duration).toLong()) }
            dragging = null
        },
        interactionSource = interaction,
        thumb = {
            SliderDefaults.Thumb(interaction, thumbSize = DpSize(4.dp, thumbHeight))
        },
        track = { state ->
            SliderDefaults.Track(
                state,
                modifier = Modifier.height(trackHeight),
                drawStopIndicator = null,
                thumbTrackGapSize = 2.dp,
            )
        },
        modifier = modifier.semantics {
            contentDescription = description
            stateDescription = state
        },
    )
}

/** A [Seekbar] with the elapsed and total time beneath. */
@Composable
fun SeekbarWithTimes(positionMs: () -> Long, durationMs: Long, onSeek: (Long) -> Unit, modifier: Modifier = Modifier) {
    Column(modifier) {
        Seekbar(positionMs, durationMs, onSeek, Modifier.fillMaxWidth(), trackHeight = 6.dp, thumbHeight = 24.dp)
        Row(Modifier.fillMaxWidth()) {
            ElapsedTime(positionMs)
            Spacer(Modifier.weight(1f))
            Text(formatTime(durationMs), style = MaterialTheme.typography.labelMedium)
        }
    }
}

@Composable
private fun ElapsedTime(positionMs: () -> Long) {
    Text(formatTime(positionMs()), style = MaterialTheme.typography.labelMedium)
}

/** m:ss, or h:mm:ss from an hour. */
fun formatTime(ms: Long): String {
    val s = ms / 1000
    return if (s >= 3600) "%d:%02d:%02d".format(s / 3600, s / 60 % 60, s % 60)
    else "%d:%02d".format(s / 60, s % 60)
}
