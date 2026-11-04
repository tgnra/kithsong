package ng.kithso.wear.ui

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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.FilledTonalButton
import androidx.wear.compose.material3.ListHeader
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.ScreenScaffold
import androidx.wear.compose.material3.Text
import ng.kithso.core.ConfirmState
import ng.kithso.core.SyncWaiting
import ng.kithso.core.WaitingDevice
import ng.kithso.wear.FileProgress
import ng.kithso.wear.R
import ng.kithso.wear.RefuseReason
import ng.kithso.wear.SyncError
import ng.kithso.wear.SyncFailure
import ng.kithso.wear.SyncState

/** One line of the sync screen; [emphasis] for the first, [error] for a failure. */
private class Line(val text: String, val emphasis: Boolean = false, val error: Boolean = false)

@Composable
fun SyncScreen(state: SyncState, onDismissRefusal: () -> Unit) {
    // Looked up here: the list's content lambda isn't composable.
    val title = stringResource(
        when (state) {
            is SyncState.Refused -> R.string.watch_pair_refused
            is SyncState.Paired, is SyncState.Unavailable -> R.string.sync
            else -> R.string.pair
        },
    )
    val lines = when (state) {
        is SyncState.Waiting -> listOf(
            Line(stringResource(R.string.watch_pair_waiting, state.deviceName), emphasis = true),
            Line(stringResource(R.string.watch_keep_open)),
        )
        is SyncState.Unavailable -> listOf(
            Line(
                stringResource(
                    when (state.error) {
                        SyncError.Open -> R.string.watch_sync_error_open
                        SyncError.Listen -> R.string.sync_error_listen
                    },
                ),
                error = true,
            ),
        )
        is SyncState.Refused -> listOf(Line(refusalText(state)))
        is SyncState.Paired -> pairedLines(state)
        is SyncState.ShowingCode -> emptyList()
    }
    val ok = stringResource(R.string.ok)
    when (state) {
        is SyncState.ShowingCode -> CodeScreen(state)
        else -> ListScreen { transform ->
            item {
                ListHeader(
                    modifier = transform.modifier(this).fillMaxWidth(),
                    transformation = transform.surface(this),
                ) {
                    Text(title)
                }
            }
            lines.forEach { line ->
                item {
                    Text(
                        line.text,
                        modifier = transform.modifier(this).fillMaxWidth()
                            .padding(horizontal = 8.dp, vertical = 4.dp),
                        style = if (line.emphasis) {
                            MaterialTheme.typography.bodyMedium
                        } else {
                            MaterialTheme.typography.bodySmall
                        },
                        color = if (line.error) MaterialTheme.colorScheme.error else Color.Unspecified,
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
                        label = { Text(ok) },
                    )
                }
            }
        }
    }
}

/**
 * In a group: what's here of what's wanted (or the files coming in), who with, whether the
 * watch's profile waits for a Sync press on another device, what waits for a device that's
 * away, and the last failure.
 */
@Composable
private fun pairedLines(state: SyncState.Paired): List<Line> {
    val lines = ArrayList<Line>()
    val progress = state.progress
    lines += Line(
        when {
            progress != null -> pluralStringResource(R.plurals.watch_syncing, progress.total, progress.done, progress.total)
            state.hasStatus -> pluralStringResource(R.plurals.watch_tracks_of, state.wanted, state.synced, state.wanted)
            else -> pluralStringResource(R.plurals.watch_tracks, state.synced, state.synced)
        },
        emphasis = true,
    )
    if (!progress?.pace.isNullOrEmpty()) lines += Line(progress.pace)
    if (state.devices.isNotEmpty()) {
        val unnamed = stringResource(R.string.device_unnamed)
        val names = state.devices.joinToString(stringResource(R.string.import_list_separator)) { it.ifEmpty { unnamed } }
        lines += Line(stringResource(R.string.watch_paired_with, names))
    }
    when (state.confirm) {
        ConfirmState.Confirmed -> Unit
        ConfirmState.Unconfirmed -> lines += Line(stringResource(R.string.watch_sync_unconfirmed))
        ConfirmState.ProfileChanged -> lines += Line(stringResource(R.string.watch_sync_paused))
    }
    waitingText(state.waiting)?.let { lines += Line(it) }
    state.failure?.let { failure ->
        lines += Line(stringResource(R.string.sync_failed_with, failure.deviceName, statusText(failure.status)), error = true)
    }
    return lines
}

/**
 * "12 tracks are waiting for Tania's desktop to come online.", as the phone's WaitingCard
 * words it, or null while nothing waits.
 */
@Composable
private fun waitingText(waiting: SyncWaiting): String? {
    val top = waiting.devices.firstOrNull() ?: return null
    val name = top.name.ifEmpty { stringResource(R.string.device_unnamed) }
    val others = waiting.devices.size - 1
    return if (others == 0) {
        pluralStringResource(R.plurals.sync_waiting, waiting.tracks, waiting.tracks, name)
    } else {
        pluralStringResource(
            R.plurals.sync_waiting_others,
            waiting.tracks,
            waiting.tracks,
            name,
            pluralStringResource(R.plurals.sync_waiting_other_devices, others, others),
        )
    }
}

/** The reason for [status], a negative mc_status, as the core words it (status_*). */
@Composable
private fun statusText(status: Int): String = stringResource(
    when (status) {
        -1 -> R.string.status_unimplemented
        -2 -> R.string.status_invalid_arg
        -3 -> R.string.status_nomem
        -4 -> R.string.status_io
        -5 -> R.string.status_db
        -6 -> R.string.status_net
        -7 -> R.string.status_format
        -8 -> R.string.status_not_found
        -9 -> R.string.status_locked
        -10 -> R.string.status_in_group
        -11 -> R.string.status_library_mismatch
        -12 -> R.string.status_auth
        else -> R.string.status_unknown
    },
)

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
                stringResource(R.string.watch_enter_code, state.peerName),
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
                    stringResource(R.string.pair_wrong_code, pluralStringResource(R.plurals.tries_left, state.attemptsLeft, state.attemptsLeft)),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                    textAlign = TextAlign.Center,
                )
            }
        }
    }
}

@Composable
private fun refusalText(state: SyncState.Refused) = when (state.reason) {
    RefuseReason.InGroup -> stringResource(R.string.watch_refused_in_group)
    RefuseReason.LibraryMismatch ->
        stringResource(R.string.watch_refused_library_mismatch, state.peerName)
    RefuseReason.AlreadyPaired -> stringResource(R.string.watch_refused_already_paired, state.peerName)
    RefuseReason.Failed -> stringResource(R.string.watch_refused_failed, state.peerName)
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
private fun PairedPreview() = SyncPreview(
    SyncState.Paired(listOf("Linux desktop", "Pixel 10"), ConfirmState.Confirmed, 12, 34, hasStatus = true),
)

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun SyncingPreview() = SyncPreview(
    SyncState.Paired(
        listOf("Linux desktop"), ConfirmState.Confirmed, 12, 34, hasStatus = true,
        progress = FileProgress(3, 22),
        waiting = SyncWaiting(5, listOf(WaitingDevice("x", "Tania's desktop", 0, 5))),
    ),
)

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun PausedPreview() = SyncPreview(
    SyncState.Paired(
        listOf("Linux desktop"), ConfirmState.ProfileChanged, 12, 0, hasStatus = false,
        failure = SyncFailure("Linux desktop", -6),
    ),
)

@Composable
private fun SyncPreview(state: SyncState) {
    MaterialTheme {
        AppScaffold {
            SyncScreen(state, onDismissRefusal = {})
        }
    }
}
