package app.musicapp.ui

import android.text.format.DateUtils
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
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
import app.musicapp.CoreSync
import app.musicapp.DeviceKind
import app.musicapp.NearbyDevice
import app.musicapp.R
import app.musicapp.SyncDevice
import app.musicapp.sync.Pairing
import app.musicapp.sync.SyncStatus
import app.musicapp.sync.Syncer

/**
 * The devices section of settings, like the desktop apps' DEVICES list: this phone, the
 * devices it syncs with, the ones on the network it could pair with, adding one by address,
 * and leaving the group. Pairing itself shows in [PairingDialog], wherever the app is.
 */
@Composable
fun DevicesSection(status: SyncStatus, syncer: Syncer) {
    var adding by rememberSaveable { mutableStateOf(false) }
    var leaving by rememberSaveable { mutableStateOf(false) }
    Column {
        SettingsHeading(stringResource(R.string.devices))
        ListItem(
            leadingContent = { Icon(Icons.Smartphone, contentDescription = null) },
            headlineContent = { Text(status.deviceName) },
            supportingContent = { Text(selfText(status)) },
        )
        notice(status)?.let { (text, isError) ->
            Text(
                text,
                style = MaterialTheme.typography.bodyMedium,
                color = if (isError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
            )
        }
        for (device in status.members) MemberRow(device, status, syncer)

        if (status.nearby.isNotEmpty()) {
            Text(
                stringResource(R.string.devices_nearby),
                style = MaterialTheme.typography.labelLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 4.dp),
            )
        }
        for (device in status.nearby) {
            ListItem(
                leadingContent = { Icon(device.kind.icon, contentDescription = null) },
                headlineContent = { Text(device.name) },
                supportingContent = {
                    Text(stringResource(if (device.inOtherGroup) R.string.device_in_other_group else R.string.device_tap_to_pair))
                },
                modifier = Modifier.clickable(enabled = !device.inOtherGroup && status.pairing == null, onClickLabel = stringResource(R.string.pair)) {
                    syncer.pair(device)
                },
            )
        }
        if (status.running && status.onLan && status.nearby.isEmpty() && !status.inGroup) {
            ListItem(
                leadingContent = { CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 2.dp) },
                headlineContent = { Text(stringResource(R.string.devices_looking)) },
                supportingContent = { Text(stringResource(R.string.devices_looking_text)) },
            )
        }

        ListItem(
            leadingContent = { Icon(Icons.Add, contentDescription = null) },
            headlineContent = { Text(stringResource(R.string.add_device_by_address)) },
            supportingContent = { Text(stringResource(R.string.add_device_by_address_text)) },
            modifier = Modifier.clickable(enabled = status.running && status.pairing == null) { adding = true },
        )
        if (status.inGroup) {
            ListItem(
                headlineContent = { Text(stringResource(R.string.leave_group), color = MaterialTheme.colorScheme.error) },
                supportingContent = { Text(stringResource(R.string.leave_group_text)) },
                modifier = Modifier.clickable(enabled = status.running) { leaving = true },
            )
        }
    }

    if (adding) {
        AddDeviceDialog(
            onConnect = { host, port ->
                adding = false
                syncer.pair(host, port)
            },
            onDismiss = { adding = false },
        )
    }
    if (leaving) {
        AlertDialog(
            onDismissRequest = { leaving = false },
            title = { Text(stringResource(R.string.leave_group_title)) },
            text = { Text(stringResource(R.string.leave_group_confirm, status.members.joinToString { it.name })) },
            confirmButton = {
                TextButton(onClick = {
                    leaving = false
                    syncer.leaveGroup()
                }) { Text(stringResource(R.string.leave)) }
            },
            dismissButton = { TextButton(onClick = { leaving = false }) { Text(stringResource(R.string.cancel)) } },
        )
    }
}

@Composable
private fun MemberRow(device: SyncDevice, status: SyncStatus, syncer: Syncer) {
    var menuOpen by remember { mutableStateOf(false) }
    var removing by remember { mutableStateOf(false) }
    val syncing = device.uid in status.syncing
    val syncingLabel = stringResource(R.string.syncing)
    Box {
        ListItem(
            leadingContent = { Icon(device.kind.icon, contentDescription = null) },
            headlineContent = { Text(device.name) },
            supportingContent = { Text(memberText(device, syncing), Modifier.semantics { liveRegion = LiveRegionMode.Polite }) },
            trailingContent = {
                if (syncing) {
                    CircularProgressIndicator(
                        Modifier.size(24.dp).semantics { contentDescription = syncingLabel },
                        strokeWidth = 2.dp,
                    )
                } else {
                    IconButton(onClick = { syncer.syncNow(device.uid) }, enabled = status.running) {
                        Icon(Icons.Sync, contentDescription = stringResource(R.string.sync_now_with, device.name))
                    }
                }
            },
            modifier = Modifier.clickable(onClickLabel = stringResource(R.string.more)) { menuOpen = true },
        )
        DropdownMenu(expanded = menuOpen, onDismissRequest = { menuOpen = false }) {
            DropdownMenuItem(
                text = { Text(stringResource(R.string.sync_now)) },
                enabled = status.running && !syncing,
                onClick = {
                    menuOpen = false
                    syncer.syncNow(device.uid)
                },
            )
            DropdownMenuItem(
                text = { Text(stringResource(R.string.remove_from_group)) },
                enabled = status.running && !device.archive,
                onClick = {
                    menuOpen = false
                    removing = true
                },
            )
        }
    }
    if (removing) {
        AlertDialog(
            onDismissRequest = { removing = false },
            title = { Text(stringResource(R.string.remove_device_title, device.name)) },
            text = { Text(stringResource(R.string.remove_device_text)) },
            confirmButton = {
                TextButton(onClick = {
                    removing = false
                    syncer.removeDevice(device.uid)
                }) { Text(stringResource(R.string.remove)) }
            },
            dismissButton = { TextButton(onClick = { removing = false }) { Text(stringResource(R.string.cancel)) } },
        )
    }
}

/**
 * Asks for a device's address and port, as the desktop apps' Add Device dialog does, for
 * networks where discovery doesn't reach it. The device then shows a code to type in here.
 */
@Composable
private fun AddDeviceDialog(onConnect: (host: String, port: Int) -> Unit, onDismiss: () -> Unit) {
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

private val DeviceKind.icon: ImageVector
    get() = when (this) {
        DeviceKind.Desktop -> Icons.Computer
        DeviceKind.Phone -> Icons.Smartphone
        DeviceKind.Watch -> Icons.Watch
    }

@Composable
private fun selfText(status: SyncStatus): String {
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
private fun notice(status: SyncStatus): Pair<String, Boolean>? = when {
    status.error != null -> status.error to true
    !status.onLan -> stringResource(R.string.sync_not_on_wifi) to true
    status.libraryState == CoreSync.LibraryState.Other -> stringResource(R.string.sync_other_library) to true
    status.libraryState == CoreSync.LibraryState.Unknown -> stringResource(R.string.sync_unknown_library) to false
    else -> null
}

@Composable
private fun memberText(device: SyncDevice, syncing: Boolean): String {
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
    return if (device.archive) stringResource(R.string.device_archive, state) else state
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
