package ng.kithso.android.ui

import android.text.format.DateUtils
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import ng.kithso.core.ConfirmState
import ng.kithso.core.CoreSync
import ng.kithso.core.DeviceKind
import ng.kithso.android.AppSettings
import ng.kithso.android.R
import ng.kithso.core.SyncDevice
import ng.kithso.android.sync.MergeProposal
import ng.kithso.android.sync.Pairing
import ng.kithso.android.sync.SyncStatus
import ng.kithso.android.sync.Syncer

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

/**
 * [PairingDialog] for the pairing under way, if any. [onChooseWhatToSync] opens a device's
 * page (a member's uid, or [THIS_DEVICE]) after pairing, to choose its sync settings.
 */
@Composable
fun PairingDialogs(syncer: Syncer, settings: AppSettings, onChooseWhatToSync: (String) -> Unit) {
    val pairing = syncer.status.collectAsState().value.pairing ?: return
    PairingDialog(pairing, syncer, settings, onChooseWhatToSync)
}

/**
 * Pairing, wherever the app is: the code another device asked for, to type in there; the code
 * this phone asked a device for, to type in here; and how it ended. Once paired, nothing
 * syncs until the device that joined has its sync settings chosen, so Choose What to Sync
 * opens its page. The first time this phone joins a group, it also says which device is the
 * archive.
 *
 * When both devices have music, the smaller library is merged into the larger. If it's this
 * phone's, its user is asked first (Merge, or Cancel to decline): in place of the code entry
 * until answered, or under the code shown. If it's the other's, a note says so.
 */
@Composable
private fun PairingDialog(pairing: Pairing, syncer: Syncer, settings: AppSettings, onChooseWhatToSync: (String) -> Unit) {
    when (pairing) {
        is Pairing.ShowCode -> {
            // The screen must not time out while someone types the code on the other device.
            val view = LocalView.current
            DisposableEffect(view) {
                view.keepScreenOn = true
                onDispose { view.keepScreenOn = false }
            }
            val merge = pairing.merge
            AlertDialog(
                onDismissRequest = {},
                title = { Text(stringResource(R.string.pair_with_question, pairing.peer)) },
                text = {
                    Column(
                        Modifier.fillMaxWidth().verticalScroll(rememberScrollState()),
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
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
                                stringResource(R.string.pair_wrong_code, triesLeft(pairing.attemptsLeft)),
                                color = MaterialTheme.colorScheme.error,
                                textAlign = TextAlign.Center,
                            )
                        }
                        if (merge != null) {
                            Spacer(Modifier.height(16.dp))
                            MergeText(merge, pairing.peer, Modifier.fillMaxWidth())
                        }
                    }
                },
                // Asked to merge: Merge beside Cancel, which declines.
                confirmButton = if (merge?.needsAnswer == true) {
                    { TextButton(onClick = syncer::confirmMerge) { Text(stringResource(R.string.pair_merge_button)) } }
                } else {
                    { TextButton(onClick = syncer::cancelPairing) { Text(stringResource(R.string.cancel)) } }
                },
                dismissButton = if (merge?.needsAnswer == true) {
                    { TextButton(onClick = syncer::cancelPairing) { Text(stringResource(R.string.cancel)) } }
                } else {
                    null
                },
            )
        }
        is Pairing.EnterCode -> {
            var code by rememberSaveable { mutableStateOf("") }
            // A wrong code: clear it for another try.
            LaunchedEffect(pairing.attemptsLeft) { if (pairing.attemptsLeft != null) code = "" }
            val submit = { if (code.length == CODE_LENGTH && !pairing.checking) syncer.enterCode(code) }
            val focus = remember { FocusRequester() }
            val merge = pairing.merge
            // Asked to merge: the proposal in place of the code entry until answered.
            val proposing = merge?.needsAnswer == true
            LaunchedEffect(proposing) { if (!proposing && !pairing.checking) focus.requestFocus() }
            if (proposing) {
                AlertDialog(
                    onDismissRequest = {},
                    title = { Text(stringResource(R.string.pair_merge_title)) },
                    text = { MergeText(merge!!, pairing.peer, Modifier.verticalScroll(rememberScrollState())) },
                    confirmButton = {
                        TextButton(onClick = syncer::confirmMerge) { Text(stringResource(R.string.pair_merge_button)) }
                    },
                    dismissButton = { TextButton(onClick = syncer::cancelPairing) { Text(stringResource(R.string.cancel)) } },
                )
                return
            }
            AlertDialog(
                onDismissRequest = {},
                title = { Text(stringResource(R.string.pair_with, pairing.peer)) },
                text = {
                    Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
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
                        if (merge != null) MergeText(merge, pairing.peer)
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
        is Pairing.Ended -> {
            val paired = pairing.reason == CoreSync.PairReason.Paired
            // The device whose sync settings are to be chosen.
            val choose = when {
                !paired -> null
                pairing.joined -> THIS_DEVICE
                else -> pairing.peerUid.ifEmpty { null }
            }
            // Which device is the archive, once: decided when the dialog first shows, and
            // kept while it does.
            val explain = rememberSaveable(pairing) {
                paired && pairing.joined && pairing.archiveName.isNotEmpty() && !settings.archiveExplained
            }
            LaunchedEffect(explain) { if (explain) settings.archiveExplained = true }
            AlertDialog(
                onDismissRequest = syncer::dismissPairing,
                title = { Text(stringResource(if (paired) R.string.paired else R.string.pair_failed)) },
                text = {
                    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                        Text(endedText(pairing))
                        if (explain) Text(archiveExplainer(pairing))
                    }
                },
                confirmButton = {
                    if (choose != null) {
                        TextButton(onClick = {
                            syncer.dismissPairing()
                            onChooseWhatToSync(choose)
                        }) { Text(stringResource(R.string.choose_what_to_sync)) }
                    } else {
                        TextButton(onClick = syncer::dismissPairing) { Text(stringResource(R.string.ok)) }
                    }
                },
                dismissButton = if (choose != null) {
                    { TextButton(onClick = syncer::dismissPairing) { Text(stringResource(R.string.ok)) } }
                } else {
                    null
                },
            )
        }
    }
}

/**
 * What the merge pairing needs means: on the phone whose library is merged, which library is
 * kept and what happens to this one, until Merge is pressed, then that it will merge; on the
 * phone whose library is kept, a note.
 */
@Composable
private fun MergeText(merge: MergeProposal, peer: String, modifier: Modifier = Modifier) {
    val here = tracksText(merge.tracksHere)
    val there = tracksText(merge.tracksThere)
    val paragraphs = when {
        merge.keptHere -> listOf(stringResource(R.string.pair_merge_kept_text, peer, here, there))
        merge.answered -> listOf(stringResource(R.string.pair_merge_confirmed))
        else -> buildList {
            add(stringResource(R.string.pair_merge_text, peer, there, here))
            add(stringResource(R.string.pair_merge_backup))
            if (merge.scrobbleUser.isNotEmpty()) add(stringResource(R.string.pair_merge_scrobble, merge.scrobbleUser, peer))
            if (merge.forgetNames.isNotEmpty()) {
                val n = merge.forgetNames.size
                val names = merge.forgetNames.joinToString(stringResource(R.string.import_list_separator))
                add(pluralStringResource(R.plurals.pair_merge_forget, n, n, names))
            }
        }
    }
    Column(modifier, verticalArrangement = Arrangement.spacedBy(12.dp)) {
        for (paragraph in paragraphs) Text(paragraph, modifier = Modifier.fillMaxWidth())
    }
}

@Composable
private fun tracksText(count: Long): String {
    val n = count.coerceIn(0, Int.MAX_VALUE.toLong()).toInt()
    return pluralStringResource(R.plurals.tracks, n, n)
}

/** Which device keeps the originals, its "**archive**" in bold. */
@Composable
private fun archiveExplainer(pairing: Pairing.Ended): AnnotatedString {
    val others = pairing.archives - 1
    val text = if (others > 0) {
        pluralStringResource(R.plurals.archive_explainer_several, others, pairing.archiveName, others)
    } else {
        stringResource(R.string.archive_explainer, pairing.archiveName)
    }
    return boldMarks(text)
}

/** [text] with each "**" pair's part in bold, and the "**" taken out. */
private fun boldMarks(text: String): AnnotatedString = buildAnnotatedString {
    val parts = text.split("**")
    parts.forEachIndexed { i, part ->
        if (i % 2 == 1) withStyle(SpanStyle(fontWeight = FontWeight.Bold)) { append(part) } else append(part)
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
    status.inGroup && status.mergePending -> stringResource(R.string.merge_pending) to false
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
    // Nothing syncs yet: the device that joined has its sync settings to choose.
    CoreSync.PairReason.Paired -> listOfNotNull(
        if (pairing.joined) {
            stringResource(R.string.paired_text, pairing.peer)
        } else {
            stringResource(R.string.paired_peer_text, pairing.peer)
        },
        // Merged: whose library was kept, and the Last.fm account signed out.
        if (!pairing.merged) {
            null
        } else if (pairing.keptHere) {
            stringResource(R.string.paired_merged_kept_text, pairing.peer)
        } else {
            stringResource(R.string.paired_merged_text, pairing.peer)
        },
        if (!pairing.merged || pairing.scrobbleSignedOut.isEmpty()) {
            null
        } else if (pairing.keptHere) {
            stringResource(R.string.paired_merged_scrobble_there, pairing.scrobbleSignedOut, pairing.peer)
        } else {
            stringResource(R.string.paired_merged_scrobble_here, pairing.scrobbleSignedOut, pairing.peer)
        },
    ).joinToString("\n\n")
    CoreSync.PairReason.MergeDeclined -> stringResource(R.string.pair_merge_declined)
    CoreSync.PairReason.WrongCode -> stringResource(R.string.pair_wrong_code_too_often)
    CoreSync.PairReason.InGroup -> stringResource(R.string.pair_in_group, pairing.peer)
    // This phone merges libraries, so the other device can't: it runs an older version.
    CoreSync.PairReason.LibraryMismatch -> stringResource(
        if (pairing.initiator) R.string.pair_library_mismatch else R.string.pair_library_mismatch_incoming,
        pairing.peer,
    ) + "\n\n" + stringResource(R.string.pair_merge_update_hint, pairing.peer)
    CoreSync.PairReason.AlreadyPaired -> stringResource(R.string.pair_already_paired, pairing.peer)
    CoreSync.PairReason.Busy -> stringResource(R.string.pair_busy, pairing.peer)
    CoreSync.PairReason.Cancelled -> stringResource(R.string.pair_cancelled)
    CoreSync.PairReason.Failed -> stringResource(R.string.pair_unreachable, pairing.peer)
}
