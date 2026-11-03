package ng.kithso.wear.ui

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.FilledTonalButton
import androidx.wear.compose.material3.ListHeader
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.Text
import ng.kithso.wear.R

/**
 * Shown before playing while no Bluetooth headphones are connected: a button to the system's
 * Bluetooth settings and, on a watch with a speaker ([hasSpeaker]), one to play through it
 * instead. Whoever shows it moves on once headphones connect or the speaker is chosen.
 */
@Composable
fun HeadphonesScreen(hasSpeaker: Boolean, onBluetoothSettings: () -> Unit, onPlayOnWatch: () -> Unit) {
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(stringResource(R.string.watch_connect_headphones))
            }
        }
        item {
            Text(
                stringResource(if (hasSpeaker) R.string.watch_headphones_text else R.string.watch_headphones_text_no_speaker),
                modifier = transform.modifier(this).fillMaxWidth().padding(horizontal = 8.dp, vertical = 4.dp),
                style = MaterialTheme.typography.bodyMedium,
                textAlign = TextAlign.Center,
            )
        }
        item {
            FilledTonalButton(
                onClick = onBluetoothSettings,
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
                label = { Text(stringResource(R.string.watch_bluetooth_settings)) },
            )
        }
        if (hasSpeaker) {
            item {
                FilledTonalButton(
                    onClick = onPlayOnWatch,
                    modifier = transform.modifier(this).fillMaxWidth(),
                    transformation = transform.surface(this),
                    label = { Text(stringResource(R.string.watch_play_on_watch)) },
                )
            }
        }
    }
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun HeadphonesScreenPreview() {
    MaterialTheme {
        AppScaffold {
            HeadphonesScreen(hasSpeaker = true, onBluetoothSettings = {}, onPlayOnWatch = {})
        }
    }
}
