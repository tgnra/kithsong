package app.kithsong.ui

import android.text.format.DateUtils
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
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
import app.kithsong.CoreSync
import app.kithsong.MusicCoreException
import app.kithsong.R
import app.kithsong.SyncDevice
import app.kithsong.sync.SyncProgress
import app.kithsong.sync.SyncStatus
import app.kithsong.sync.Syncer
import java.text.NumberFormat
import kotlinx.coroutines.launch

/** What [SyncScreen] passes to open this phone's own [DeviceScreen], rather than a member's uid. */
const val THIS_DEVICE = ""

/**
 * The sync screen, like the desktop apps' DEVICES list: this phone, the devices it syncs with,
 * the ones on the network it could pair with, adding one by address, and leaving the group.
 * Tapping a device, this phone included, calls [onOpenDevice] with its uid ([THIS_DEVICE] for
 * this phone). Pairing itself shows in [PairingDialogs], wherever the app is. [onBack] slides
 * the screen away; it is also dismissed by swiping right.
 */
@Composable
fun SyncScreen(syncer: Syncer, bottomPadding: Dp, onOpenDevice: (String) -> Unit, onBack: () -> Unit) {
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
    // While files come in, how many of them.
    val files = status.sessions[device.uid]?.takeIf { it.filesTotal > 0 }?.let { filesText(it) }
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
// the files once there are some to fetch.
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
        progress.takeIf { it.filesTotal > 0 }?.let { filesText(it) },
        progress.takeIf { it.bytesReceived > 0 || it.bytesSent > 0 }?.let {
            stringResource(
                R.string.progress_bytes,
                sizeText(context, it.bytesReceived),
                sizeText(context, it.bytesSent),
            )
        },
    )
    ListItem(
        headlineContent = { Text(title ?: phase) },
        supportingContent = {
            Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
                if (details.isNotEmpty()) Text(details.joinToString(stringResource(R.string.separator_dot)))
                if (progress.filesTotal > 0) {
                    LinearProgressIndicator(
                        progress = { progress.filesDone.toFloat() / progress.filesTotal },
                        modifier = Modifier.fillMaxWidth(),
                    )
                } else {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                }
            }
        },
    )
}

// "3 of 10 files", fetched so far in a session.
@Composable
private fun filesText(progress: SyncProgress): String =
    pluralStringResource(R.plurals.sync_notification_files, progress.filesTotal, progress.filesDone, progress.filesTotal)

// How the last session with a member went.
@Composable
private fun resultText(result: CoreSync.Event.SyncDone): String = when {
    result.status != 0 -> stringResource(R.string.sync_failed, statusText(LocalContext.current, result.status))
    !result.librarySynced -> stringResource(R.string.sync_device_list_only)
    else -> listOfNotNull(
        pluralStringResource(R.plurals.session_files, result.filesReceived, result.filesReceived, result.filesSent),
        result.filesMissing.takeIf { it > 0 }?.let { stringResource(R.string.files_missing, it.toString()) },
        result.filesCorrupt.takeIf { it > 0 }?.let { stringResource(R.string.files_damaged, it.toString()) },
    ).joinToString(stringResource(R.string.separator_dot))
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
