package app.musicapp.wear.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.FilledTonalButton
import androidx.wear.compose.material3.ListHeader
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.ScreenScaffold
import androidx.wear.compose.material3.Text
import app.musicapp.wear.RefuseReason
import app.musicapp.wear.SyncState

@Composable
fun SyncScreen(state: SyncState, onDismissRefusal: () -> Unit) {
    when (state) {
        is SyncState.ShowingCode -> CodeScreen(state)
        else -> ListScreen { transform ->
            item {
                ListHeader(
                    modifier = transform.modifier(this).fillMaxWidth(),
                    transformation = transform.surface(this),
                ) {
                    Text(
                        when (state) {
                            is SyncState.Refused -> "Can't pair"
                            is SyncState.Paired -> "Sync"
                            else -> "Pair"
                        },
                    )
                }
            }
            val lines = when (state) {
                is SyncState.Waiting -> listOf(
                    "Add ${state.deviceName} under Devices on your computer or phone.",
                    "Keep this screen open.",
                )
                is SyncState.Refused -> listOf(refusalText(state))
                is SyncState.Paired -> listOf(
                    when {
                        state.synced < state.wanted ->
                            "Syncing ${state.synced} of ${state.wanted} tracks"
                        else -> "${state.wanted} tracks on this watch"
                    },
                    "Paired with ${state.devices.joinToString()}. Choose what this watch " +
                        "holds on one of them.",
                )
                is SyncState.ShowingCode -> emptyList()
            }
            lines.forEachIndexed { i, line ->
                item {
                    Text(
                        line,
                        modifier = transform.modifier(this).fillMaxWidth()
                            .padding(horizontal = 8.dp, vertical = 4.dp),
                        style = if (i == 0) {
                            MaterialTheme.typography.bodyMedium
                        } else {
                            MaterialTheme.typography.bodySmall
                        },
                        textAlign = TextAlign.Center,
                    )
                }
            }
            if (state is SyncState.Refused) {
                item {
                    FilledTonalButton(
                        onClick = onDismissRefusal,
                        modifier = transform.modifier(this).fillMaxWidth(),
                        transformation = transform.surface(this),
                        label = { Text("OK") },
                    )
                }
            }
        }
    }
}

/** The pairing code, large, for the other device's user to type in. */
@Composable
private fun CodeScreen(state: SyncState.ShowingCode) {
    // The screen must not time out while someone types the code on the other device.
    val view = LocalView.current
    DisposableEffect(view) {
        view.keepScreenOn = true
        onDispose { view.keepScreenOn = false }
    }
    ScreenScaffold {
        Column(
            modifier = Modifier.fillMaxSize().padding(horizontal = 24.dp),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                "Enter this code on ${state.peerName}",
                style = MaterialTheme.typography.bodyMedium,
                textAlign = TextAlign.Center,
            )
            Spacer(Modifier.height(8.dp))
            Text(
                state.code.chunked(3).joinToString(" "),
                style = MaterialTheme.typography.numeralSmall,
                color = MaterialTheme.colorScheme.primary,
            )
            if (state.attemptsLeft < 3) {
                Spacer(Modifier.height(8.dp))
                Text(
                    "Wrong code. ${state.attemptsLeft} " +
                        (if (state.attemptsLeft == 1) "try" else "tries") + " left.",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}

private fun refusalText(state: SyncState.Refused) = when (state.reason) {
    RefuseReason.InGroup ->
        "This watch is already paired with other devices. Remove it there first."
    RefuseReason.LibraryMismatch ->
        "This watch holds music from a different library than ${state.peerName}."
    RefuseReason.AlreadyPaired -> "This watch is already paired with ${state.peerName}."
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun WaitingPreview() = SyncPreview(SyncState.Waiting("Galaxy Watch7"))

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun CodePreview() = SyncPreview(SyncState.ShowingCode("Linux desktop", "042917", 2))

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun RefusedPreview() =
    SyncPreview(SyncState.Refused("Linux desktop", RefuseReason.InGroup))

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun PairedPreview() = SyncPreview(SyncState.Paired(listOf("Linux desktop"), 12, 34))

@Composable
private fun SyncPreview(state: SyncState) {
    MaterialTheme {
        AppScaffold {
            SyncScreen(state, onDismissRefusal = {})
        }
    }
}
