package ng.kithso.android.ui

import android.text.format.DateUtils
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.compose.LocalActivity
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
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
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import ng.kithso.core.CoreSync
import ng.kithso.android.R
import ng.kithso.core.SyncDevice
import ng.kithso.core.SyncProblems
import ng.kithso.core.SyncWaiting
import ng.kithso.core.TranscodeTarget
import ng.kithso.android.sync.LostRemoval
import ng.kithso.android.sync.SyncProgress
import ng.kithso.android.sync.SyncStatus
import ng.kithso.android.sync.Syncer
import java.text.NumberFormat
import kotlinx.coroutines.launch
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ListItemColors
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.LocalContentColor
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.window.Dialog

/**
 * The Sync page: the other devices of the group, then what this device keeps, which only it
 * changes, with Leave group at the end. With [twoPanes] (the tablet layout) this device's
 * settings are on the left and the devices on the right, under a strip with what the last sync
 * did and Sync now, as the desktops have it; otherwise one column, the devices a row of cards
 * whose + shows the unpaired ones, and Sync now is the pill the app shows in the mini player's
 * place. A device shows its settings and details in [DeviceDialog]. Pairing itself shows in
 * [PairingDialogs], wherever the app is.
 */
@Composable
fun SyncScreen(
    syncer: Syncer,
    bottomPadding: Dp,
    onBack: () -> Unit,
    onOpenBin: () -> Unit,
    twoPanes: Boolean = false,
) {
    val status by syncer.status.collectAsState()
    var adding by rememberSaveable { mutableStateOf(false) }
    var unpaired by rememberSaveable { mutableStateOf(false) }
    // The device whose dialog is open: a member's uid, or this device's own.
    var shown by rememberSaveable { mutableStateOf<String?>(null) }
    val takeOver = rememberTakeOver(syncer)
    ArchiveOfferDialog(syncer, tablet = twoPanes)
    val padding = pageSides().add(WindowInsets(bottom = bottomPadding)).asPaddingValues()
    val notice = syncNotice(status)
    val selfHeading = stringResource(if (twoPanes) R.string.this_tablet else R.string.this_phone)
    val notices: LazyListScope.() -> Unit = {
        if (status.transcodingOnBattery) item(key = "transcoding") {
            TranscodingNotice(syncer, Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
        }
        if (status.binCount > 0) item(key = "bin") {
            BinNotice(status.binCount, onOpenBin, Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
        }
        notice?.let { (text, isError) ->
            item(key = "notice") {
                Text(
                    text,
                    style = MaterialTheme.typography.bodyMedium,
                    color = if (isError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp),
                )
            }
        }
        // A lost archive this device may replace, once the one-time dialog was shown.
        status.offer?.takeIf { it.asked }?.let { offer ->
            item(key = "offer") {
                ArchiveOfferCard(offer, tablet = twoPanes, onTakeOver = takeOver, modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
            }
        }
    }
    // This device: its name and how it's reached, for its details, then its settings.
    val self: LazyListScope.() -> Unit = {
        item(key = "self") {
            SettingsHeading(selfHeading)
            val selfWarning = status.self?.warning.orEmpty()
            ListItem(
                leadingContent = { Icon(Icons.Smartphone, contentDescription = null) },
                headlineContent = { Text(status.deviceName) },
                supportingContent = {
                    Column {
                        Text(selfText(status))
                        if (selfWarning.isNotEmpty()) Text(selfWarning, color = MaterialTheme.colorScheme.error)
                    }
                },
                modifier = Modifier.clickable(onClickLabel = stringResource(R.string.device_details)) { shown = THIS_DEVICE },
            )
        }
        item(key = "profile") {
            val me = status.self
            if (me != null) ProfileSection(me, isSelf = true, syncer) else Note(stringResource(R.string.profile_no_group))
        }
    }
    val leave: LazyListScope.() -> Unit = {
        if (status.inGroup) item(key = "leave") { LeaveGroupRow(status, syncer) }
    }

    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        if (twoPanes) {
            Column(Modifier.fillMaxSize()) {
                ScreenHeader(stringResource(R.string.sync), onBack)
                if (status.inGroup) {
                    Surface(
                        shape = RoundedCornerShape(20.dp),
                        color = MaterialTheme.colorScheme.surfaceContainerHigh,
                        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                    ) {
                        SyncPillContent(
                            status, onSyncNow = { syncer.syncNow() }, onCancel = { syncer.cancelSync() },
                            onDetails = rememberSyncIssues(syncer), nothingToDo = syncer.nothingToDo,
                        )
                    }
                }
                Row(Modifier.weight(1f)) {
                    LazyColumn(contentPadding = padding, modifier = Modifier.weight(1f).fillMaxHeight()) {
                        notices()
                        self()
                    }
                    LazyColumn(contentPadding = padding, modifier = Modifier.weight(1f).fillMaxHeight()) {
                        item(key = "devices") { SettingsHeading(stringResource(R.string.devices)) }
                        items(status.members, key = { "member:${it.uid}" }) { device ->
                            MemberRow(device, status) { shown = device.uid }
                        }
                        if (status.members.isEmpty()) item(key = "none") { Note(stringResource(R.string.sync_no_devices_tablet)) }
                        if (status.nearby.isNotEmpty()) item(key = "nearby") { SettingsHeading(stringResource(R.string.devices_nearby)) }
                        item(key = "unpaired") {
                            UnpairedRows(status, syncer, onAdd = { adding = true }, onPair = {}, asked = false)
                        }
                        leave()
                    }
                }
            }
        } else {
            LazyColumn(contentPadding = padding, modifier = Modifier.fillMaxSize()) {
                item(key = "header") { ScreenHeader(stringResource(R.string.sync), onBack) }
                notices()
                item(key = "devices") {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Box(Modifier.weight(1f)) { SettingsHeading(stringResource(R.string.devices)) }
                        IconButton(onClick = { unpaired = true }, enabled = status.running, modifier = Modifier.padding(top = 16.dp, end = 4.dp)) {
                            Icon(Icons.Add, contentDescription = stringResource(R.string.add_device))
                        }
                    }
                    if (status.members.isEmpty()) {
                        Note(stringResource(R.string.sync_no_devices))
                    } else {
                        LazyRow(
                            contentPadding = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                        ) {
                            items(status.members, key = { it.uid }) { device ->
                                MemberCard(device, syncing = device.uid in status.syncing) { shown = device.uid }
                            }
                        }
                    }
                }
                self()
                leave()
            }
        }
    }

    if (unpaired) {
        AlertDialog(
            onDismissRequest = { unpaired = false },
            title = { Text(stringResource(R.string.devices_nearby)) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState())) {
                    UnpairedRows(
                        status,
                        syncer,
                        onAdd = {
                            unpaired = false
                            adding = true
                        },
                        onPair = { unpaired = false },
                        asked = true,
                        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                    )
                }
            },
            confirmButton = { TextButton(onClick = { unpaired = false }) { Text(stringResource(R.string.close)) } },
        )
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
    shown?.let { uid -> DeviceDialog(syncer, uid, onDismiss = { shown = null }) }
}

/** What [SyncScreen] passes to [DeviceDialog] for this device's own, rather than a member's uid. */
private const val THIS_DEVICE = ""

/**
 * The devices on this network that aren't in the group, to pair with ([onPair] after asking),
 * and Add device by address ([onAdd]). While there are none it says it's looking, outside a
 * group or when they were asked for ([asked]).
 */
@Composable
private fun UnpairedRows(
    status: SyncStatus,
    syncer: Syncer,
    onAdd: () -> Unit,
    onPair: () -> Unit,
    asked: Boolean,
    colors: ListItemColors = ListItemDefaults.colors(),
) {
    Column {
        for (device in status.nearby) {
            ListItem(
                leadingContent = { Icon(device.kind.icon, contentDescription = null) },
                headlineContent = { Text(device.name) },
                supportingContent = {
                    Text(
                        device.versionText.ifEmpty {
                            stringResource(if (device.inOtherGroup) R.string.device_in_other_group else R.string.device_tap_to_pair)
                        },
                    )
                },
                colors = colors,
                modifier = Modifier.clickable(enabled = !device.inOtherGroup && status.pairing == null, onClickLabel = stringResource(R.string.pair)) {
                    syncer.pair(device)
                    onPair()
                },
            )
        }
        if (status.running && status.onLan && status.nearby.isEmpty() && (asked || !status.inGroup)) {
            ListItem(
                leadingContent = { CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 2.dp) },
                headlineContent = { Text(stringResource(R.string.devices_looking)) },
                supportingContent = { Text(stringResource(R.string.devices_looking_text)) },
                colors = colors,
            )
        }
        ListItem(
            leadingContent = { Icon(Icons.Add, contentDescription = null) },
            headlineContent = { Text(stringResource(R.string.add_device_by_address)) },
            supportingContent = { Text(stringResource(R.string.add_device_by_address_text)) },
            colors = colors,
            modifier = Modifier.clickable(enabled = status.running && status.pairing == null, onClick = onAdd),
        )
    }
}

/** Leave group, which asks first, naming the devices this one would stop syncing with. */
@Composable
private fun LeaveGroupRow(status: SyncStatus, syncer: Syncer) {
    var leaving by rememberSaveable { mutableStateOf(false) }
    ListItem(
        headlineContent = { Text(stringResource(R.string.leave_group), color = MaterialTheme.colorScheme.error) },
        supportingContent = { Text(stringResource(R.string.leave_group_text)) },
        modifier = Modifier.padding(top = 16.dp).clickable(enabled = status.running) { leaving = true },
    )
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

// A member in the two panes: what it syncs, and whether something's wrong with it or a session runs.
@Composable
private fun MemberRow(device: SyncDevice, status: SyncStatus, onOpen: () -> Unit) {
    val syncing = device.uid in status.syncing
    val warning = device.warning.takeIf { !syncing && it.isNotEmpty() }
    ListItem(
        leadingContent = { MemberIcon(device, syncing, warning != null) },
        headlineContent = { Text(device.name) },
        supportingContent = {
            Column(Modifier.semantics { liveRegion = LiveRegionMode.Polite }) {
                Text(status.summaries[device.uid].orEmpty())
                if (warning != null) Text(warning, color = MaterialTheme.colorScheme.error)
            }
        },
        modifier = Modifier.clickable(onClickLabel = stringResource(R.string.device_details), onClick = onOpen),
    )
}

// A member on the phone: a card with its icon and name, the archive's in the accent colour.
@Composable
private fun MemberCard(device: SyncDevice, syncing: Boolean, onOpen: () -> Unit) {
    val archive = stringResource(R.string.device_archive_title)
    Card(
        onClick = onOpen,
        colors = if (device.archive) {
            CardDefaults.cardColors(
                containerColor = MaterialTheme.colorScheme.primaryContainer,
                contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
            )
        } else {
            CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerHigh)
        },
        modifier = Modifier.width(120.dp).semantics { if (device.archive) stateDescription = archive },
    ) {
        Column(
            Modifier.fillMaxWidth().padding(12.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            MemberIcon(device, syncing, warning = !syncing && device.warning.isNotEmpty())
            Text(
                device.name,
                style = MaterialTheme.typography.labelLarge,
                textAlign = TextAlign.Center,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

// A member's kind, a spinner in its place while a session runs, in the error colour with a [warning].
@Composable
private fun MemberIcon(device: SyncDevice, syncing: Boolean, warning: Boolean) {
    if (syncing) {
        val syncingLabel = stringResource(R.string.syncing)
        CircularProgressIndicator(
            Modifier.size(24.dp).semantics { contentDescription = syncingLabel },
            strokeWidth = 2.dp,
        )
    } else {
        Icon(
            device.kind.icon,
            contentDescription = null,
            tint = if (warning) MaterialTheme.colorScheme.error else LocalContentColor.current,
        )
    }
}

/**
 * A device's sync settings, read-only for a member (only it changes them), and its details:
 * how it is, its files, where it's reached, and Remove from group, but for the archive (the
 * core refuses: it holds the originals). For this device ([THIS_DEVICE]), the details only,
 * with Rename; its settings are on the page. A member that leaves the group closes it.
 */
@Composable
private fun DeviceDialog(syncer: Syncer, uid: String, onDismiss: () -> Unit) {
    val status by syncer.status.collectAsState()
    val isSelf = uid == THIS_DEVICE
    val device = status.members.find { it.uid == uid }
    LaunchedEffect(isSelf, device == null) { if (!isSelf && device == null) onDismiss() }
    var renaming by rememberSaveable { mutableStateOf(false) }
    var removing by rememberSaveable { mutableStateOf(false) }
    val member = if (isSelf) status.self else device
    val issuesUid = member?.uid
    val showIssues = rememberSyncIssues(syncer, issuesUid)
    val hasIssues = issuesUid != null && shownIssues(status.problems.issues, issuesUid).isNotEmpty()
    if (!isSelf && device == null) return

    Dialog(onDismissRequest = onDismiss) {
        Surface(shape = MaterialTheme.shapes.extraLarge, color = MaterialTheme.colorScheme.surface) {
            LazyColumn(contentPadding = PaddingValues(vertical = 8.dp)) {
                item(key = "title") {
                    Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(start = 24.dp, end = 4.dp, top = 8.dp)) {
                        Text(device?.name ?: status.deviceName, style = MaterialTheme.typography.headlineSmall, modifier = Modifier.weight(1f))
                        IconButton(onClick = onDismiss) { Icon(Icons.Clear, contentDescription = stringResource(R.string.close)) }
                    }
                }
                // What's wrong with it (its version, a hold on its side, its reach...), and the details.
                val warning = member?.warning.orEmpty()
                if (warning.isNotEmpty() || hasIssues) item(key = "warning") {
                    Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp)) {
                        if (warning.isNotEmpty()) {
                            Text(warning, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error)
                        }
                        if (hasIssues) {
                            TextButton(onClick = showIssues, modifier = Modifier.align(Alignment.End)) {
                                Text(stringResource(R.string.sync_issues_details))
                            }
                        }
                    }
                }
                item(key = "info") {
                    if (device == null) {
                        InfoRow(stringResource(R.string.device_address), addressText(status.address?.takeIf { status.running }, status.port))
                        status.self?.reachText?.takeIf { it.isNotEmpty() }?.let { InfoRow(stringResource(R.string.device_reach), it) }
                        status.self?.let { FilesRows(it) }
                        // This device's sessions with any member.
                        status.sessions.forEach { (memberUid, progress) ->
                            val name = status.members.find { it.uid == memberUid }?.name ?: stringResource(R.string.sync_other_device)
                            SessionRow(progress, stringResource(R.string.sync_notification_syncing_with, name))
                        }
                        ListItem(
                            headlineContent = { Text(stringResource(R.string.rename)) },
                            supportingContent = { Text(status.deviceName) },
                            modifier = Modifier.clickable(enabled = status.running) { renaming = true },
                        )
                    } else {
                        val syncing = device.uid in status.syncing
                        InfoRow(stringResource(R.string.device_status), memberText(device, syncing))
                        status.sessions[device.uid]?.let { SessionRow(it, null) }
                        if (!syncing) status.results[device.uid]?.let { InfoRow(stringResource(R.string.device_last_session), resultText(it)) }
                        if (!syncing && device.note.isNotEmpty()) InfoRow(stringResource(R.string.device_next_sync), device.note)
                        if (device.reachText.isNotEmpty()) InfoRow(stringResource(R.string.device_reach), device.reachText)
                        FilesRows(device)
                        InfoRow(stringResource(R.string.device_address), addressText(device.address, device.port))
                    }
                }
                if (device != null) {
                    item(key = "profile") {
                        SettingsHeading(stringResource(R.string.device_profile))
                        ProfileSection(device, isSelf = false, syncer)
                    }
                    if (!device.archive) item(key = "remove") {
                        ListItem(
                            headlineContent = { Text(stringResource(R.string.remove_from_group), color = MaterialTheme.colorScheme.error) },
                            modifier = Modifier.clickable(enabled = status.running) { removing = true },
                        )
                    }
                }
            }
        }
    }

    if (renaming && isSelf) {
        RenameDialog(
            stringResource(R.string.rename_this_phone),
            status.deviceName,
            onRename = { name ->
                renaming = false
                syncer.rename(name)
            },
            onDismiss = { renaming = false },
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

// A line of explanation in place of what isn't there.
@Composable
private fun Note(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
    )
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
    result.cancelled || result.peerCancelled -> stringResource(R.string.sync_cancelled)
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

/**
 * While this phone transcodes for another member on battery, which goes on only while a sync
 * screen is open: stay here or plug in a charger, and it may use more battery. Background lets
 * it go on off screen until it's done ([Syncer.keepInBackground]), after which the notice says so.
 */
@Composable
fun TranscodingNotice(syncer: Syncer, modifier: Modifier = Modifier) {
    val background by remember(syncer) { syncer.status.map { it.background }.distinctUntilChanged() }
        .collectAsState(syncer.status.value.background)
    val targets by remember(syncer) {
        syncer.status.map { status -> transcodeTargets(status.transcodingFor, status.members) }.distinctUntilChanged()
    }.collectAsState(emptyList())
    val lines = targets.map { (tracks, name) -> pluralStringResource(R.plurals.sync_transcoding_for, tracks, tracks, name) }
    TranscodingNotice(background, onBackground = syncer::keepInBackground, modifier, lines)
}

/**
 * Who [targets] are for, by the names of [members], with how many tracks each: those of a
 * member that isn't one (any more), or that didn't say, are left out.
 */
fun transcodeTargets(targets: List<TranscodeTarget>, members: List<SyncDevice>): List<Pair<Int, String>> =
    targets.mapNotNull { target ->
        members.firstOrNull { it.uid == target.uid }?.let { target.tracks to it.name }
    }

/**
 * [TranscodingNotice] for [background] as it stands, with [onBackground] for its button. [lines]:
 * "Transcoding 42 tracks for Desk", one per member, the first in the title's place; none
 * while that isn't known, for the plain title.
 */
@Composable
fun TranscodingNotice(
    background: Boolean,
    onBackground: () -> Unit,
    modifier: Modifier = Modifier,
    lines: List<String> = emptyList(),
) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.secondaryContainer,
            contentColor = MaterialTheme.colorScheme.onSecondaryContainer,
        ),
        modifier = modifier.fillMaxWidth().semantics { liveRegion = LiveRegionMode.Polite },
    ) {
        Column(Modifier.padding(horizontal = 16.dp, vertical = 12.dp)) {
            Text(lines.firstOrNull() ?: stringResource(R.string.sync_transcoding_title), style = MaterialTheme.typography.titleMedium)
            for (line in lines.drop(1)) Text(line, style = MaterialTheme.typography.titleSmall)
            Text(
                stringResource(if (background) R.string.sync_transcoding_background_text else R.string.sync_transcoding_text),
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.padding(top = 4.dp),
            )
            if (!background) {
                TextButton(onClick = onBackground, modifier = Modifier.align(Alignment.End)) {
                    Text(stringResource(R.string.sync_transcoding_background), color = MaterialTheme.colorScheme.onSecondaryContainer)
                }
            }
        }
    }
}

/**
 * Tells [syncer] whether a sync screen is open ([shown]: the Sync page or one under it) while the
 * app is resumed, the only time this phone transcodes for other members on battery. The pages of
 * the sync section share it, so moving between them doesn't withdraw it. Leaving the app or
 * turning the screen off withdraws it from the lifecycle event itself, as no frame may come to
 * recompose. While it transcodes on battery the screen stays on, unless Background was pressed,
 * after which it needn't.
 */
@Composable
fun SyncSectionShown(syncer: Syncer, shown: Boolean) {
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    val currentShown by rememberUpdatedState(shown)
    DisposableEffect(lifecycle, syncer) {
        val observer = LifecycleEventObserver { _, _ ->
            syncer.setSyncScreenShown(currentShown && lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED))
        }
        lifecycle.addObserver(observer)
        onDispose {
            lifecycle.removeObserver(observer)
            syncer.setSyncScreenShown(false)
        }
    }
    LaunchedEffect(shown) {
        syncer.setSyncScreenShown(shown && lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED))
    }
    val onBattery by remember(syncer) {
        syncer.status.map { it.transcodingOnBattery && !it.background }.distinctUntilChanged()
    }.collectAsState(false)
    val window = LocalActivity.current?.window
    val keepOn = shown && onBattery
    DisposableEffect(window, keepOn) {
        if (keepOn) window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        onDispose { if (keepOn) window?.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON) }
    }
}

/**
 * What's wrong with sync, with a way to remove what no device has any more, and Details, which
 * lists every problem with how to fix it.
 */
@Composable
fun ProblemsCard(problems: SyncProblems, syncer: Syncer, modifier: Modifier = Modifier) {
    val onRemove: suspend (List<String>) -> LostRemoval = syncer::removeLost
    val showIssues = rememberSyncIssues(syncer)
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
                Row {
                    if (shownIssues(problems.issues).isNotEmpty()) {
                        TextButton(onClick = showIssues) {
                            Text(stringResource(R.string.sync_issues_details), color = MaterialTheme.colorScheme.onErrorContainer)
                        }
                    }
                    if (problems.lost.isNotEmpty()) {
                        TextButton(onClick = { asking = true }) {
                            Text(stringResource(R.string.sync_lost_remove), color = MaterialTheme.colorScheme.onErrorContainer)
                        }
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
internal fun LostDialog(problems: SyncProblems, onRemove: suspend (List<String>) -> LostRemoval, onDismiss: () -> Unit) {
    val context = LocalContext.current
    val resources = LocalResources.current
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
                            is LostRemoval.Kept -> resources.getQuantityString(R.plurals.sync_lost_kept, outcome.tracks, outcome.tracks)
                            LostRemoval.Busy -> resources.getString(R.string.sync_lost_busy)
                            LostRemoval.Failed -> resources.getString(R.string.sync_lost_failed)
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
