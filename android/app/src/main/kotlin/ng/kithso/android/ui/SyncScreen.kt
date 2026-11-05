package ng.kithso.android.ui

import android.text.format.DateUtils
import android.widget.Toast
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.core.CoreSync
import ng.kithso.core.MusicCoreException
import ng.kithso.android.R
import ng.kithso.core.SyncDevice
import ng.kithso.core.SyncProblems
import ng.kithso.core.SyncWaiting
import ng.kithso.android.sync.LostRemoval
import ng.kithso.android.sync.SyncProgress
import ng.kithso.android.sync.SyncStatus
import ng.kithso.android.sync.Syncer
import java.text.NumberFormat
import kotlinx.coroutines.launch

/** What [SyncScreen] passes to open this phone's own [DeviceScreen], rather than a member's uid. */
const val THIS_DEVICE = ""

/**
 * The sync screen, like the desktop apps' DEVICES list: this phone, the devices it syncs with,
 * the ones on the network it could pair with, adding one by address, and leaving the group.
 * Tapping a device, this phone included, calls [onOpenDevice] with its uid ([THIS_DEVICE] for
 * this phone). Pairing itself shows in [PairingDialogs], wherever the app is. [onBack] slides
 * the screen away; it is also dismissed by swiping right. While deletes wait in the recycle bin,
 * a notice at the top says the phone only syncs when Sync now is pressed, which makes them
 * permanent; [onOpenBin] opens the bin from it.
 */
@Composable
fun SyncScreen(
    syncer: Syncer,
    bottomPadding: Dp,
    onOpenDevice: (String) -> Unit,
    onBack: () -> Unit,
    onOpenBin: () -> Unit = {},
) {
    val status by syncer.status.collectAsState()
    var adding by rememberSaveable { mutableStateOf(false) }
    var leaving by rememberSaveable { mutableStateOf(false) }
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            contentPadding = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
                .add(WindowInsets(bottom = bottomPadding))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "header") { ScreenHeader(stringResource(R.string.sync), onBack) }

            if (status.binCount > 0) item(key = "bin") {
                BinNotice(status.binCount, onOpenBin, Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            }

            item(key = "self") {
                syncNotice(status)?.let { (text, isError) ->
                    Text(
                        text,
                        style = MaterialTheme.typography.bodyMedium,
                        color = if (isError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
                    )
                }
                SettingsHeading(stringResource(R.string.this_phone))
                ListItem(
                    leadingContent = { Icon(Icons.Smartphone, contentDescription = null) },
                    headlineContent = { Text(status.deviceName) },
                    supportingContent = { Text(selfText(status)) },
                    modifier = Modifier.clickable(onClickLabel = stringResource(R.string.device_details)) {
                        onOpenDevice(THIS_DEVICE)
                    },
                )
                SettingsHeading(stringResource(R.string.devices))
            }

            items(status.members, key = { "member:${it.uid}" }) { device ->
                MemberRow(device, status, syncer) { onOpenDevice(device.uid) }
            }
            if (status.members.isNotEmpty()) item(key = "sync_now") {
                val syncing = status.syncing.isNotEmpty()
                ListItem(
                    leadingContent = {
                        if (syncing) CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 2.dp)
                        else Icon(Icons.Sync, contentDescription = null)
                    },
                    headlineContent = { Text(stringResource(if (syncing) R.string.syncing else R.string.sync_now)) },
                    supportingContent = { Text(stringResource(R.string.sync_now_text)) },
                    trailingContent = { if (syncing) CancelSyncButton { syncer.cancelSync() } },
                    modifier = Modifier.clickable(enabled = status.running && !syncing) { syncer.syncNow() },
                )
            }

            if (status.nearby.isNotEmpty()) item(key = "nearby") {
                Text(
                    stringResource(R.string.devices_nearby),
                    style = MaterialTheme.typography.labelLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 4.dp),
                )
            }
            items(status.nearby, key = { "nearby:${it.uid}" }) { device ->
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
            if (status.running && status.onLan && status.nearby.isEmpty() && !status.inGroup) item(key = "looking") {
                ListItem(
                    leadingContent = { CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 2.dp) },
                    headlineContent = { Text(stringResource(R.string.devices_looking)) },
                    supportingContent = { Text(stringResource(R.string.devices_looking_text)) },
                )
            }

            item(key = "actions") {
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

// A member of the group: tapping it opens its page, and the button beside it syncs with it.
@Composable
private fun MemberRow(device: SyncDevice, status: SyncStatus, syncer: Syncer, onOpen: () -> Unit) {
    val syncing = device.uid in status.syncing
    val syncingLabel = stringResource(R.string.syncing)
    // While files come in or go out, how many of them.
    val files = status.sessions[device.uid]?.takeIf { it.moving > 0 }?.let { filesText(it) }
    val text = listOfNotNull(memberText(device, syncing), files).joinToString(stringResource(R.string.separator_dot))
    ListItem(
        leadingContent = { Icon(device.kind.icon, contentDescription = null) },
        headlineContent = { Text(device.name) },
        supportingContent = { Text(text, Modifier.semantics { liveRegion = LiveRegionMode.Polite }) },
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
        modifier = Modifier.clickable(onClickLabel = stringResource(R.string.device_details), onClick = onOpen),
    )
}

/**
 * One device's page, opened from [SyncScreen]: what sync knows of it and its files, sessions
 * with it under way, syncing with it, renaming it or removing it from the group, and its
 * profile ([ProfileSection]). [uid] is a member's, or [THIS_DEVICE] for this phone. A member
 * that leaves the group closes the page with [onBack], as does back or a swipe right.
 * [onRename], if given, renames this phone; a member is renamed through its profile.
 */
@Composable
fun DeviceScreen(
    syncer: Syncer,
    uid: String,
    bottomPadding: Dp,
    onBack: () -> Unit,
    onRename: ((String) -> Unit)? = null,
) {
    val status by syncer.status.collectAsState()
    val isSelf = uid == THIS_DEVICE
    val device = status.members.find { it.uid == uid }
    // Removed here or by another device, or the group was left.
    val gone = !isSelf && device == null
    LaunchedEffect(gone) { if (gone) onBack() }
    var renaming by rememberSaveable { mutableStateOf(false) }
    var renameError by rememberSaveable { mutableStateOf<String?>(null) }
    var removing by rememberSaveable { mutableStateOf(false) }
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            contentPadding = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
                .add(WindowInsets(bottom = bottomPadding))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "header") { ScreenHeader(device?.name ?: status.deviceName, onBack) }

            item(key = "info") {
                if (device == null) {
                    ListItem(
                        leadingContent = { Icon(Icons.Smartphone, contentDescription = null) },
                        headlineContent = { Text(stringResource(R.string.this_phone)) },
                        supportingContent = { Text(stringResource(if (status.inGroup) R.string.this_phone_in_group else R.string.this_phone_alone)) },
                    )
                    InfoRow(stringResource(R.string.device_address), addressText(status.address?.takeIf { status.running }, status.port))
                    // After pairing by merging, until the old library's music has come in.
                    if (status.mergePending) {
                        Text(
                            stringResource(R.string.merge_pending),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                        )
                    }
                    status.self?.let { FilesRows(it) }
                    // This phone's sessions with any member.
                    status.sessions.forEach { (memberUid, progress) ->
                        val name = status.members.find { it.uid == memberUid }?.name ?: stringResource(R.string.sync_other_device)
                        SessionRow(progress, stringResource(R.string.sync_notification_syncing_with, name))
                    }
                    if (onRename != null) {
                        ListItem(
                            headlineContent = { Text(stringResource(R.string.rename)) },
                            supportingContent = { Text(status.deviceName) },
                            modifier = Modifier.clickable(enabled = status.running) { renaming = true },
                        )
                    }
                } else {
                    val syncing = device.uid in status.syncing
                    ListItem(
                        leadingContent = { Icon(device.kind.icon, contentDescription = null) },
                        headlineContent = { Text(stringResource(R.string.device_status)) },
                        supportingContent = {
                            Text(
                                when {
                                    syncing -> stringResource(R.string.syncing)
                                    device.online -> stringResource(R.string.device_online)
                                    else -> stringResource(R.string.device_status_offline)
                                },
                                Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                            )
                        },
                    )
                    status.sessions[device.uid]?.let { SessionRow(it, null) }
                    InfoRow(stringResource(R.string.device_last_synced), lastSyncText(device.lastSyncMs))
                    if (!syncing) status.results[device.uid]?.let { InfoRow(stringResource(R.string.device_last_session), resultText(it)) }
                    FilesRows(device)
                    InfoRow(stringResource(R.string.device_address), addressText(device.address, device.port))
                    if (device.archive) InfoRow(stringResource(R.string.device_archive_title), stringResource(R.string.device_archive_text))
                    ListItem(
                        headlineContent = { Text(stringResource(R.string.rename)) },
                        supportingContent = { Text(device.name) },
                        modifier = Modifier.clickable { renaming = true },
                    )

                    ListItem(
                        leadingContent = {
                            if (syncing) CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 2.dp)
                            else Icon(Icons.Sync, contentDescription = null)
                        },
                        headlineContent = { Text(stringResource(if (syncing) R.string.syncing else R.string.sync_now)) },
                        trailingContent = { if (syncing) CancelSyncButton { syncer.cancelSync(device.uid) } },
                        modifier = Modifier.clickable(enabled = status.running && !syncing) { syncer.syncNow(device.uid) },
                    )
                    // The group's archive can't be removed (the core refuses): it holds the originals.
                    if (!device.archive) {
                        ListItem(
                            headlineContent = { Text(stringResource(R.string.remove_from_group), color = MaterialTheme.colorScheme.error) },
                            modifier = Modifier.clickable(enabled = status.running) { removing = true },
                        )
                    }
                }
            }

            item(key = "profile") {
                SettingsHeading(stringResource(R.string.device_profile))
                val member = if (isSelf) status.self else device
                if (member != null) {
                    ProfileSection(member, isSelf, syncer)
                } else {
                    Text(
                        stringResource(R.string.profile_no_group),
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.fillMaxWidth().padding(16.dp),
                    )
                }
            }
        }
    }

    if (renaming && device != null) {
        RenameDialog(
            stringResource(R.string.rename_device, device.name),
            device.name,
            onRename = { name ->
                renaming = false
                scope.launch {
                    try {
                        syncer.editProfile(device.uid) { it.copy(name = name) }
                    } catch (e: MusicCoreException) {
                        renameError = statusText(context, e.status)
                    }
                }
            },
            onDismiss = { renaming = false },
        )
    } else if (renaming && onRename != null) {
        RenameDialog(
            stringResource(R.string.rename_this_phone),
            status.deviceName,
            onRename = { name ->
                renaming = false
                onRename(name)
            },
            onDismiss = { renaming = false },
        )
    }
    renameError?.let { reason ->
        AlertDialog(
            onDismissRequest = { renameError = null },
            text = { Text(stringResource(R.string.device_rename_failed, reason)) },
            confirmButton = { TextButton(onClick = { renameError = null }) { Text(stringResource(R.string.ok)) } },
        )
    }
    if (removing && device != null) {
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

// The device's own report of its files: how many of those it should hold are there and
// checked, missing or damaged, their size, and when it last checked them.
@Composable
private fun FilesRows(device: SyncDevice) {
    val context = LocalContext.current
    if (!device.hasStatus) {
        InfoRow(stringResource(R.string.device_files), stringResource(R.string.device_files_none))
        return
    }
    val number = NumberFormat.getIntegerInstance()
    val expected = device.expected.coerceAtMost(Int.MAX_VALUE.toLong()).toInt()
    val parts = listOfNotNull(
        pluralStringResource(R.plurals.files_ok, expected, number.format(device.ok), number.format(device.expected)),
        device.missing.takeIf { it > 0 }?.let { stringResource(R.string.files_missing, number.format(it)) },
        device.corrupt.takeIf { it > 0 }?.let { stringResource(R.string.files_damaged, number.format(it)) },
        sizeText(context, device.bytes),
    )
    InfoRow(stringResource(R.string.device_files), parts.joinToString(stringResource(R.string.import_list_separator)))
    if (device.verifiedMs > 0) InfoRow(stringResource(R.string.device_files_checked), lastSyncText(device.verifiedMs))
}

// A session under way: [title] (else its step) over its step, files and bytes, and a bar for
// the files once there are some to fetch or send.
@Composable
private fun SessionRow(progress: SyncProgress, title: String?) {
    val context = LocalContext.current
    val phase = stringResource(
        when (progress.phase) {
            CoreSync.Phase.Connecting -> R.string.phase_connecting
            CoreSync.Phase.Group -> R.string.phase_group
            CoreSync.Phase.Library -> R.string.phase_library
            CoreSync.Phase.Files -> R.string.phase_files
            CoreSync.Phase.Done -> R.string.phase_done
        },
    )
    val details = listOfNotNull(
        phase.takeIf { title != null },
        progress.takeIf { it.moving > 0 }?.let { filesText(it) },
        progress.takeIf { it.bytesReceived > 0 || it.bytesSent > 0 }?.let {
            stringResource(
                R.string.progress_bytes,
                sizeText(context, it.bytesReceived),
                sizeText(context, it.bytesSent),
            )
        },
        progress.pace.takeIf { it.isNotEmpty() },
    )
    ListItem(
        headlineContent = { Text(title ?: phase) },
        supportingContent = {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (details.isNotEmpty()) Text(details.joinToString(stringResource(R.string.separator_dot)))
                if (progress.moving > 0) {
                    LinearProgressIndicator(
                        progress = { progress.moved.toFloat() / progress.moving },
                        modifier = Modifier.fillMaxWidth(),
                    )
                } else {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                }
            }
        },
    )
}

// "3 of 10 files", fetched so far in a session, or "3 of 10 files sent" in the member's turn.
@Composable
private fun filesText(progress: SyncProgress): String =
    if (progress.sending) {
        pluralStringResource(R.plurals.sync_notification_files_sent, progress.sendsTotal, progress.sendsDone, progress.sendsTotal)
    } else {
        pluralStringResource(R.plurals.sync_notification_files, progress.filesTotal, progress.filesDone, progress.filesTotal)
    }

// How the last session with a member went.
@Composable
private fun resultText(result: CoreSync.Event.SyncDone): String = when {
    result.cancelled -> stringResource(R.string.sync_cancelled)
    result.status != 0 -> stringResource(R.string.sync_failed, statusText(LocalContext.current, result.status))
    !result.librarySynced -> stringResource(R.string.sync_device_list_only)
    else -> listOfNotNull(
        pluralStringResource(R.plurals.session_files, result.filesReceived, result.filesReceived, result.filesSent),
        result.filesMissing.takeIf { it > 0 }?.let { stringResource(R.string.files_missing, it.toString()) },
        result.filesCorrupt.takeIf { it > 0 }?.let { stringResource(R.string.files_damaged, it.toString()) },
    ).joinToString(stringResource(R.string.separator_dot))
}

// Beside "Syncing…": ends the session (see [Syncer.cancelSync]).
@Composable
private fun CancelSyncButton(onClick: () -> Unit) {
    TextButton(onClick = onClick) { Text(stringResource(R.string.cancel_sync)) }
}

@Composable
private fun InfoRow(title: String, value: String) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(value) },
    )
}

/** Asks for a new name for a device, starting from [name]. */
@Composable
private fun RenameDialog(title: String, name: String, onRename: (String) -> Unit, onDismiss: () -> Unit) {
    var text by rememberSaveable { mutableStateOf(name) }
    val valid = text.isNotBlank()
    val rename = { if (valid) onRename(text.trim()) }
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            OutlinedTextField(
                value = text,
                onValueChange = { text = it },
                label = { Text(stringResource(R.string.name)) },
                singleLine = true,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { rename() }),
                modifier = Modifier.fillMaxWidth().focusRequester(focus),
            )
        },
        confirmButton = { TextButton(onClick = rename, enabled = valid) { Text(stringResource(R.string.rename)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

@Composable
private fun lastSyncText(ms: Long): String = if (ms <= 0) {
    stringResource(R.string.device_never_synced)
} else {
    DateUtils.formatDateTime(
        LocalContext.current,
        ms,
        DateUtils.FORMAT_SHOW_DATE or DateUtils.FORMAT_SHOW_TIME or DateUtils.FORMAT_SHOW_YEAR,
    )
}

@Composable
private fun addressText(address: String?, port: Int): String = when {
    address == null -> stringResource(R.string.device_address_unknown)
    port != CoreSync.DEFAULT_PORT && port != 0 -> stringResource(R.string.device_address_port, address, port)
    else -> address
}

/**
 * Under the main list's header while tracks of this phone's sync settings can only come from
 * members it can't reach now: "12 tracks are waiting for Tania's desktop to come online."
 * Calm, with nothing to press or dismiss: it goes by itself once they've arrived.
 */
/**
 * Deletes made here wait in the recycle bin ([count] of them), so this phone doesn't sync by
 * itself: Sync now, below, sends them for good. [onOpenBin] opens the bin, to restore any.
 */
@Composable
private fun BinNotice(count: Int, onOpenBin: () -> Unit, modifier: Modifier = Modifier) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.errorContainer,
            contentColor = MaterialTheme.colorScheme.onErrorContainer,
        ),
        modifier = modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 4.dp)) {
            Text(stringResource(R.string.sync_bin_title), style = MaterialTheme.typography.titleMedium)
            Text(
                pluralStringResource(R.plurals.sync_bin_text, count, count),
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(top = 4.dp),
            )
            TextButton(onClick = onOpenBin, modifier = Modifier.align(Alignment.End)) {
                Text(stringResource(R.string.sync_bin_open), color = MaterialTheme.colorScheme.onErrorContainer)
            }
        }
    }
}

/** What's wrong with the files here, with a way to remove what no device has any more. */
@Composable
fun ProblemsCard(problems: SyncProblems, onRemove: suspend (List<String>) -> LostRemoval, modifier: Modifier = Modifier) {
    var asking by rememberSaveable { mutableStateOf(false) }
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
        modifier = modifier.fillMaxWidth(),
    ) {
        Row(Modifier.padding(start = 16.dp, top = 12.dp, end = 16.dp, bottom = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(
                Icons.Warning,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onErrorContainer,
                modifier = Modifier.padding(end = 12.dp).size(20.dp),
            )
            Column {
                Text(
                    problems.text,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onErrorContainer,
                    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                )
                if (problems.lost.isNotEmpty()) {
                    TextButton(onClick = { asking = true }) {
                        Text(stringResource(R.string.sync_lost_remove), color = MaterialTheme.colorScheme.onErrorContainer)
                    }
                }
            }
        }
    }
    if (asking && problems.lost.isNotEmpty()) {
        LostDialog(problems, onRemove, onDismiss = { asking = false })
    }
}

// How many of the tracks the confirmation names.
private const val LOST_NAMED = 6

/** Asks before removing the tracks no device has any more, naming them. It can't be undone. */
@Composable
private fun LostDialog(problems: SyncProblems, onRemove: suspend (List<String>) -> LostRemoval, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val lost = problems.lost
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(pluralStringResource(R.plurals.sync_lost_confirm_title, lost.size, lost.size)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(stringResource(R.string.sync_lost_confirm_body))
                Column {
                    lost.take(LOST_NAMED).forEach { track ->
                        Text(if (track.artist.isEmpty()) track.title else stringResource(R.string.sync_lost_track, track.title, track.artist))
                    }
                    val more = lost.size - LOST_NAMED
                    if (more > 0) Text(pluralStringResource(R.plurals.sync_lost_confirm_more, more, more))
                }
            }
        },
        confirmButton = {
            TextButton(
                onClick = {
                    onDismiss()
                    scope.launch {
                        val text = when (val outcome = onRemove(lost.map { it.uid })) {
                            LostRemoval.Removed -> null
                            is LostRemoval.Kept -> context.resources.getQuantityString(R.plurals.sync_lost_kept, outcome.tracks, outcome.tracks)
                            LostRemoval.Busy -> context.getString(R.string.sync_lost_busy)
                            LostRemoval.Failed -> context.getString(R.string.sync_lost_failed)
                        }
                        if (text != null) Toast.makeText(context, text, Toast.LENGTH_LONG).show()
                    }
                },
            ) { Text(stringResource(R.string.sync_lost_confirm_button)) }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

@Composable
fun WaitingCard(waiting: SyncWaiting, modifier: Modifier = Modifier) {
    val top = waiting.devices.firstOrNull() ?: return
    val name = top.name.ifEmpty { stringResource(R.string.device_unnamed) }
    val others = waiting.devices.size - 1
    val text = if (others == 0) {
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
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerHigh),
        modifier = modifier.fillMaxWidth(),
    ) {
        Row(Modifier.padding(horizontal = 16.dp, vertical = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(
                Icons.Sync,
                contentDescription = null,
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(end = 12.dp).size(20.dp),
            )
            Text(
                text,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
            )
        }
    }
}
