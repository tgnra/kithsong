package app.musicapp.ui

import android.text.format.DateUtils
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import app.musicapp.ConfirmState
import app.musicapp.CoreSync
import app.musicapp.DeviceKind
import app.musicapp.R
import app.musicapp.SyncDevice
import app.musicapp.sync.Pairing
import app.musicapp.sync.SyncStatus
import app.musicapp.sync.Syncer

/**
 * Asks for a device's address and port, as the desktop apps' Add Device dialog does, for
 * networks where discovery doesn't reach it. The device then shows a code to type in here.
 */
@Composable
fun AddDeviceDialog(onConnect: (host: String, port: Int) -> Unit, onDismiss: () -> Unit) {
    var host by rememberSaveable { mutableStateOf("") }
    var port by rememberSaveable { mutableStateOf(CoreSync.DEFAULT_PORT.toString()) }
    val portNumber = port.toIntOrNull()?.takeIf { it in 1..65535 }
    val valid = host.isNotBlank() && portNumber != null
    val connect = { if (valid) onConnect(host.trim(), portNumber!!) }
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.add_device)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(stringResource(R.string.add_device_text))
                OutlinedTextField(
                    value = host,
                    onValueChange = { host = it },
                    label = { Text(stringResource(R.string.add_device_host)) },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri, imeAction = ImeAction.Next),
                    modifier = Modifier.fillMaxWidth().focusRequester(focus),
                )
                OutlinedTextField(
                    value = port,
                    onValueChange = { value -> port = value.filter(Char::isDigit).take(5) },
                    label = { Text(stringResource(R.string.add_device_port)) },
                    singleLine = true,
                    isError = portNumber == null,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Go),
                    keyboardActions = KeyboardActions(onGo = { connect() }),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
        },
        confirmButton = { TextButton(onClick = connect, enabled = valid) { Text(stringResource(R.string.connect)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

/** [PairingDialog] for the pairing under way, if any. */
@Composable
fun PairingDialogs(syncer: Syncer) {
    val pairing = syncer.status.collectAsState().value.pairing ?: return
    PairingDialog(pairing, syncer)
}

/**
 * Pairing, wherever the app is: the code another device asked for, to type in there; the code
 * this phone asked a device for, to type in here; and how it ended.
 */
@Composable
private fun PairingDialog(pairing: Pairing, syncer: Syncer) {
    when (pairing) {
        is Pairing.ShowCode -> {
            // The screen must not time out while someone types the code on the other device.
            val view = LocalView.current
            DisposableEffect(view) {
                view.keepScreenOn = true
                onDispose { view.keepScreenOn = false }
            }
            AlertDialog(
                onDismissRequest = {},
                title = { Text(stringResource(R.string.pair_with_question, pairing.peer)) },
                text = {
                    Column(Modifier.fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(stringResource(R.string.pair_show_code, pairing.peer), modifier = Modifier.fillMaxWidth())
                        Spacer(Modifier.height(16.dp))
                        val codeDescription = stringResource(R.string.pair_code_description, pairing.code.toList().joinToString(" "))
                        Text(
                            pairing.code.chunked(3).joinToString(" "),
                            style = MaterialTheme.typography.displayMedium,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.semantics {
                                contentDescription = codeDescription
                                liveRegion = LiveRegionMode.Polite
                            },
                        )
                        if (pairing.attemptsLeft < MAX_ATTEMPTS) {
                            Spacer(Modifier.height(8.dp))
                            Text(
                                stringResource(R.string.pair_wrong_code_entered, triesLeft(pairing.attemptsLeft)),
                                color = MaterialTheme.colorScheme.error,
                                textAlign = TextAlign.Center,
                            )
                        }
                    }
                },
                confirmButton = { TextButton(onClick = syncer::cancelPairing) { Text(stringResource(R.string.cancel)) } },
            )
        }
        is Pairing.EnterCode -> {
            var code by rememberSaveable { mutableStateOf("") }
            // A wrong code: clear it for another try.
            LaunchedEffect(pairing.attemptsLeft) { if (pairing.attemptsLeft != null) code = "" }
            val submit = { if (code.length == CODE_LENGTH && !pairing.checking) syncer.enterCode(code) }
            val focus = remember { FocusRequester() }
            LaunchedEffect(Unit) { focus.requestFocus() }
            AlertDialog(
                onDismissRequest = {},
                title = { Text(stringResource(R.string.pair_with, pairing.peer)) },
                text = {
                    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        Text(stringResource(R.string.pair_enter_code, pairing.peer))
                        OutlinedTextField(
                            value = code,
                            onValueChange = { value -> code = value.filter(Char::isDigit).take(CODE_LENGTH) },
                            label = { Text(stringResource(R.string.pair_code)) },
                            singleLine = true,
                            enabled = !pairing.checking,
                            isError = pairing.attemptsLeft != null,
                            supportingText = pairing.attemptsLeft?.let { left -> { Text(stringResource(R.string.pair_wrong_code, triesLeft(left))) } },
                            textStyle = MaterialTheme.typography.headlineSmall,
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.NumberPassword, imeAction = ImeAction.Done),
                            keyboardActions = KeyboardActions(onDone = { submit() }),
                            modifier = Modifier.fillMaxWidth().focusRequester(focus),
                        )
                    }
                },
                confirmButton = {
                    TextButton(onClick = submit, enabled = code.length == CODE_LENGTH && !pairing.checking) {
                        Text(stringResource(if (pairing.checking) R.string.pairing else R.string.pair))
                    }
                },
                dismissButton = { TextButton(onClick = syncer::cancelPairing) { Text(stringResource(R.string.cancel)) } },
            )
        }
        is Pairing.Ended -> AlertDialog(
            onDismissRequest = syncer::dismissPairing,
            title = { Text(stringResource(if (pairing.reason == CoreSync.PairReason.Paired) R.string.paired else R.string.pair_failed)) },
            text = { Text(endedText(pairing)) },
            confirmButton = { TextButton(onClick = syncer::dismissPairing) { Text(stringResource(R.string.ok)) } },
        )
    }
}

private const val CODE_LENGTH = 6
private const val MAX_ATTEMPTS = 3

@Composable
private fun triesLeft(n: Int) = pluralStringResource(R.plurals.tries_left, n, n)

val DeviceKind.icon: ImageVector
    get() = when (this) {
        DeviceKind.Desktop -> Icons.Computer
        DeviceKind.Phone -> Icons.Smartphone
        DeviceKind.Watch -> Icons.Watch
    }

@Composable
fun selfText(status: SyncStatus): String {
    val self = stringResource(if (status.inGroup) R.string.self_in_group else R.string.self_alone)
    val address = status.address?.takeIf { status.running } ?: return self
    return if (status.port != CoreSync.DEFAULT_PORT && status.port != 0) {
        stringResource(R.string.self_address_port, self, address, status.port)
    } else {
        stringResource(R.string.self_address, self, address)
    }
}

// What may stop sync, and whether it's a problem rather than a remark.
@Composable
fun syncNotice(status: SyncStatus): Pair<String, Boolean>? = when {
    status.error != null -> status.error to true
    !status.onLan -> stringResource(R.string.sync_not_on_wifi) to true
    status.libraryState == CoreSync.LibraryState.Other -> stringResource(R.string.sync_other_library) to true
    status.libraryState == CoreSync.LibraryState.Unknown -> stringResource(R.string.sync_unknown_library) to false
    status.inGroup && status.selfConfirm != null -> confirmText(status.selfConfirm)?.let { it to false }
    else -> null
}

/** Why files don't sync on their own for a device, or null if they do. */
@Composable
private fun confirmText(confirm: ConfirmState): String? = when (confirm) {
    ConfirmState.Confirmed -> null
    ConfirmState.Unconfirmed -> stringResource(R.string.sync_unconfirmed)
    ConfirmState.ProfileChanged -> stringResource(R.string.sync_paused_profile_changed)
}

@Composable
fun memberText(device: SyncDevice, syncing: Boolean): String {
    val now = System.currentTimeMillis()
    val last = when {
        device.lastSyncMs <= 0 -> null
        now - device.lastSyncMs < DateUtils.MINUTE_IN_MILLIS -> stringResource(R.string.synced_just_now)
        else -> DateUtils.getRelativeTimeSpanString(device.lastSyncMs, now, DateUtils.MINUTE_IN_MILLIS).toString()
    }
    val state = when {
        syncing -> stringResource(R.string.syncing)
        device.online -> if (last != null) stringResource(R.string.device_online_synced, last) else stringResource(R.string.device_online)
        else -> if (last != null) stringResource(R.string.device_offline_synced, last) else stringResource(R.string.device_not_synced)
    }
    val shown = if (device.archive) stringResource(R.string.device_archive, state) else state
    return confirmText(device.confirm)?.let { "$shown\n$it" } ?: shown
}

@Composable
private fun endedText(pairing: Pairing.Ended): String = when (pairing.reason) {
    CoreSync.PairReason.Paired -> stringResource(R.string.paired_text, pairing.peer)
    CoreSync.PairReason.WrongCode -> stringResource(R.string.pair_wrong_code_too_often)
    CoreSync.PairReason.InGroup -> stringResource(R.string.pair_in_group, pairing.peer)
    CoreSync.PairReason.LibraryMismatch -> stringResource(
        if (pairing.initiator) R.string.pair_library_mismatch else R.string.pair_library_mismatch_incoming,
        pairing.peer,
    )
    CoreSync.PairReason.AlreadyPaired -> stringResource(R.string.pair_already_paired, pairing.peer)
    CoreSync.PairReason.Busy -> stringResource(R.string.pair_busy, pairing.peer)
    CoreSync.PairReason.Cancelled -> stringResource(R.string.pair_cancelled)
    CoreSync.PairReason.Failed -> stringResource(R.string.pair_unreachable, pairing.peer)
}
