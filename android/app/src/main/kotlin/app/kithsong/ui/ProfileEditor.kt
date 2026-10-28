package app.kithsong.ui

import android.content.Context
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemColors
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.PrimaryScrollableTabRow
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusManager
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.window.DialogWindowProvider
import androidx.core.view.WindowCompat
import app.kithsong.CoreSync
import app.kithsong.MusicCoreException
import app.kithsong.ProfileItem
import app.kithsong.ProfileItemKind
import app.kithsong.R
import app.kithsong.SyncDevice
import app.kithsong.SyncEstimate
import app.kithsong.SyncProfile
import app.kithsong.data.ProfileChoice
import app.kithsong.data.ProfileChoices
import app.kithsong.sync.Syncer
import java.text.NumberFormat
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Edits made in the profile editor and not stored yet: only the fields changed and the items
 * ticked (true) or unticked (false). They're applied over the profile as stored, both to show
 * it and to store it, so edits made meanwhile on other devices to other fields or items stay,
 * as on the Windows app's device page.
 */
@Immutable
private data class ProfileEdits(
    val opusKbps: Int? = null,
    val budgetEnabled: Boolean? = null,
    val budgetBytes: Long? = null,
    val syncAll: Boolean? = null,
    val items: Map<ProfileItem, Boolean> = emptyMap(),
) {
    val isEmpty: Boolean get() = this == None

    fun applyTo(profile: SyncProfile): SyncProfile = profile.copy(
        opusKbps = opusKbps ?: profile.opusKbps,
        budgetEnabled = budgetEnabled ?: profile.budgetEnabled,
        budgetBytes = budgetBytes ?: profile.budgetBytes,
        syncAll = syncAll ?: profile.syncAll,
        items = profile.items.filter { items[it] != false } +
            items.filter { (item, on) -> on && item !in profile.items }.keys,
    )

    companion object {
        val None = ProfileEdits()
    }
}

/**
 * What [device] holds, editable: everything or a selection, as originals or Opus, within a
 * size budget, or the whole library as originals if it's an archive; with how much that comes
 * to, worked out again as it's edited. [isSelf]: it's this phone. Edits are stored a moment
 * after the last one, as on the Windows app's device page, and any left over as the page
 * closes are stored then; failures show here.
 */
@Composable
fun ProfileSection(device: SyncDevice, isSelf: Boolean, syncer: Syncer) {
    val uid = device.uid
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var stored by remember(uid) { mutableStateOf<SyncProfile?>(null) }
    var readError by remember(uid) { mutableStateOf<Int?>(null) }
    var edits by remember(uid) { mutableStateOf(ProfileEdits.None) }
    var message by remember(uid) { mutableStateOf<String?>(null) }
    var picking by rememberSaveable(uid) { mutableStateOf(false) }
    var archiving by rememberSaveable(uid) { mutableStateOf(false) }

    // Read again whenever what sync knows of the device changes, which may come with edits
    // made on another device.
    LaunchedEffect(device) {
        try {
            stored = syncer.profile(uid)
            readError = null
        } catch (e: MusicCoreException) {
            readError = e.status
        }
    }
    val profile = remember(stored, edits) { stored?.let(edits::applyTo) }

    LaunchedEffect(edits) {
        if (edits.isEmpty) return@LaunchedEffect
        delay(APPLY_DELAY_MS)
        val pending = edits
        message = try {
            stored = syncer.editProfile(uid, pending::applyTo)
            null
        } catch (e: MusicCoreException) {
            if (e.status == CoreSync.ERR_LOCKED) {
                context.getString(R.string.profile_locked)
            } else {
                context.getString(R.string.profile_save_failed, statusText(context, e.status))
            }
        }
        edits = ProfileEdits.None
    }
    val latestEdits by rememberUpdatedState(edits)
    DisposableEffect(uid) {
        onDispose {
            val pending = latestEdits
            if (!pending.isEmpty) syncer.editProfileLater(uid, pending::applyTo)
        }
    }

    var estimate by remember(uid) { mutableStateOf<SyncEstimate?>(null) }
    var estimateError by remember(uid) { mutableStateOf<Int?>(null) }
    var estimating by remember(uid) { mutableStateOf(false) }
    LaunchedEffect(profile) {
        val candidate = profile ?: return@LaunchedEffect
        estimating = true
        delay(ESTIMATE_DELAY_MS)
        try {
            estimate = syncer.estimate(uid, candidate)
            estimateError = null
        } catch (e: MusicCoreException) {
            estimateError = e.status
        }
        estimating = false
    }

    if (profile == null) {
        Note(readError?.let { stringResource(R.string.profile_read_failed, statusText(context, it)) })
        return
    }
    val editable = !profile.archive
    val edit: (ProfileEdits.() -> ProfileEdits) -> Unit = { change ->
        message = null
        edits = edits.change()
    }
    val setArchive: (Boolean) -> Unit = { archive ->
        scope.launch {
            message = try {
                stored = syncer.setArchive(uid, archive)
                null
            } catch (e: MusicCoreException) {
                if (e.status == CoreSync.ERR_LOCKED && !archive && isSelf) {
                    context.getString(R.string.profile_archive_locked)
                } else {
                    context.getString(R.string.profile_archive_failed, statusText(context, e.status))
                }
            }
        }
    }

    Column {
        message?.let { text ->
            Text(
                text,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 4.dp)
                    .semantics { liveRegion = LiveRegionMode.Polite },
            )
        }

        EstimateCard(profile, estimate, estimateError, estimating)

        // Only the device itself may stop being an archive.
        val archiveEnabled = !profile.archive || isSelf
        ListItem(
            headlineContent = { Text(stringResource(R.string.device_archive_title)) },
            supportingContent = {
                Text(
                    if (profile.archive && !isSelf) stringResource(R.string.profile_archive_other, device.name)
                    else stringResource(R.string.device_archive_text),
                )
            },
            trailingContent = { Switch(checked = profile.archive, onCheckedChange = null, enabled = archiveEnabled) },
            modifier = Modifier.toggleable(profile.archive, enabled = archiveEnabled, role = Role.Switch) { on ->
                if (on) archiving = true else setArchive(false)
            },
        )

        SubHeading(stringResource(R.string.what_to_sync))
        ListItem(
            headlineContent = { Text(stringResource(R.string.sync_everything)) },
            colors = itemColors(editable),
            trailingContent = { Switch(checked = profile.syncAll, onCheckedChange = null, enabled = editable) },
            modifier = Modifier.toggleable(profile.syncAll, enabled = editable, role = Role.Switch) { on ->
                edit { copy(syncAll = on) }
            },
        )
        val choosable = editable && !profile.syncAll
        ListItem(
            headlineContent = { Text(stringResource(R.string.profile_choose)) },
            colors = itemColors(choosable),
            supportingContent = { Text(selectionText(profile.items)) },
            modifier = Modifier.clickable(enabled = choosable) { picking = true },
        )

        // Auto and a budget are one choice: a budget picks the format, and picking one drops the budget.
        SubHeading(stringResource(R.string.sync_format))
        Column(Modifier.selectableGroup()) {
            ListItem(
                leadingContent = { RadioButton(selected = profile.budgetEnabled, onClick = null, enabled = editable) },
                headlineContent = { Text(stringResource(R.string.format_auto)) },
                colors = itemColors(editable),
                modifier = Modifier.selectable(profile.budgetEnabled, enabled = editable, role = Role.RadioButton) {
                    edit { copy(budgetEnabled = true, budgetBytes = profile.budgetBytes.takeIf { it > 0 } ?: (DEFAULT_BUDGET_GB * GIB)) }
                },
            )
            (listOf(0) + CoreSync.OPUS_BITRATES).forEachIndexed { i, kbps ->
                val selected = !profile.budgetEnabled && profile.opusKbps == kbps
                ListItem(
                    leadingContent = { RadioButton(selected = selected, onClick = null, enabled = editable) },
                    headlineContent = { Text(formatText(kbps)) },
                    colors = itemColors(editable),
                    trailingContent = {
                        estimate?.stepBytes?.getOrNull(i)?.let { Text(sizeText(context, it)) }
                    },
                    modifier = Modifier.selectable(selected, enabled = editable, role = Role.RadioButton) {
                        edit { copy(opusKbps = kbps, budgetEnabled = false) }
                    },
                )
            }
        }

        // Turning the budget off leaves Auto for originals.
        BudgetRows(profile, editable) { enabled, bytes ->
            edit { copy(budgetEnabled = enabled, budgetBytes = bytes, opusKbps = if (enabled) opusKbps else 0) }
        }
    }

    if (picking) {
        ProfileItemsDialog(
            selected = profile.items.toSet(),
            onToggle = { item, on -> edit { copy(items = items + (item to on)) } },
            onDismiss = { picking = false },
            syncer = syncer,
        )
    }
    if (archiving) {
        MakeArchiveDialog(
            device.name,
            uid,
            profile,
            syncer,
            onConfirm = {
                archiving = false
                setArchive(true)
            },
            onDismiss = { archiving = false },
        )
    }
}

// The size budget: a switch, and while it's on, how many GB. The number shows the stored one
// until it's edited, and goes back to it if left empty.
@Composable
private fun BudgetRows(profile: SyncProfile, editable: Boolean, onChange: (Boolean, Long) -> Unit) {
    val storedGb = if (profile.budgetBytes > 0) ((profile.budgetBytes + GIB / 2) / GIB).coerceAtLeast(1) else DEFAULT_BUDGET_GB
    var text by remember { mutableStateOf(storedGb.toString()) }
    var focused by remember { mutableStateOf(false) }
    LaunchedEffect(storedGb) { if (!focused) text = storedGb.toString() }
    ListItem(
        headlineContent = { Text(stringResource(R.string.budget_limit)) },
        colors = itemColors(editable),
        trailingContent = { Switch(checked = profile.budgetEnabled, onCheckedChange = null, enabled = editable) },
        modifier = Modifier.toggleable(profile.budgetEnabled, enabled = editable, role = Role.Switch) { on ->
            onChange(on, storedGb * GIB)
        },
    )
    if (!profile.budgetEnabled) return
    val focusManager: FocusManager = LocalFocusManager.current
    OutlinedTextField(
        value = text,
        onValueChange = { value ->
            text = value.filter(Char::isDigit).take(6)
            text.toLongOrNull()?.takeIf { it > 0 }?.let { onChange(true, it * GIB) }
        },
        enabled = editable,
        singleLine = true,
        suffix = { Text(stringResource(R.string.budget_unit)) },
        isError = text.toLongOrNull()?.takeIf { it > 0 } == null,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number, imeAction = ImeAction.Done),
        keyboardActions = KeyboardActions(onDone = { focusManager.clearFocus() }),
        modifier = Modifier
            .padding(start = 16.dp, end = 16.dp, bottom = 8.dp)
            .width(160.dp)
            .onFocusChanged { state ->
                if (focused && !state.isFocused && text.toLongOrNull()?.takeIf { it > 0 } == null) text = storedGb.toString()
                focused = state.isFocused
            },
    )
}

// How much the profile comes to: tracks and size, the format a budget picks or how far over
// it is, and the room there is.
@Composable
private fun EstimateCard(profile: SyncProfile, estimate: SyncEstimate?, error: Int?, estimating: Boolean) {
    val context = LocalContext.current
    fun size(bytes: Long) = sizeText(context, bytes)
    Surface(
        shape = RoundedCornerShape(12.dp),
        border = BorderStroke(1.dp, MaterialTheme.colorScheme.outlineVariant),
        color = MaterialTheme.colorScheme.surfaceContainerLow,
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
    ) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.Top) {
            Column(
                Modifier.weight(1f).semantics { liveRegion = LiveRegionMode.Polite },
                verticalArrangement = Arrangement.spacedBy(4.dp),
            ) {
                when {
                    estimate != null -> {
                        Text(
                            pluralStringResource(R.plurals.estimate_tracks, estimate.tracks, estimate.tracks, size(estimate.totalBytes)),
                            style = MaterialTheme.typography.titleSmall,
                        )
                        if (estimate.overBudget) {
                            Text(
                                stringResource(R.string.estimate_over_budget, size(estimate.shortfallBytes)),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.error,
                            )
                        } else if (profile.budgetEnabled && !estimate.archive) {
                            SecondaryText(stringResource(R.string.estimate_fits_as, formatText(estimate.kbps)))
                        }
                        val room = listOfNotNull(
                            estimate.limitBytes.takeIf { it >= 0 }?.let { stringResource(R.string.estimate_limit, size(it)) },
                            estimate.freeBytes.takeIf { it >= 0 }?.let { stringResource(R.string.estimate_free, size(it)) },
                        )
                        if (room.isNotEmpty()) SecondaryText(room.joinToString(stringResource(R.string.separator_dot)))
                    }
                    error != null -> SecondaryText(stringResource(R.string.estimate_failed, statusText(context, error)))
                    else -> SecondaryText(stringResource(R.string.measuring))
                }
            }
            if (estimating) CircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
        }
    }
}

/** Confirms making [name] an archive, with how much the whole library comes to as originals. */
@Composable
private fun MakeArchiveDialog(
    name: String,
    uid: String,
    profile: SyncProfile,
    syncer: Syncer,
    onConfirm: () -> Unit,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current
    var bytes by remember { mutableStateOf<Long?>(null) }
    LaunchedEffect(uid) {
        bytes = try {
            syncer.estimate(uid, profile.copy(archive = true)).totalBytes
        } catch (e: MusicCoreException) {
            null
        }
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.make_archive_title, name)) },
        text = {
            Text(
                bytes?.let { stringResource(R.string.make_archive_text, sizeText(context, it)) }
                    ?: stringResource(R.string.make_archive_text_unknown),
            )
        },
        confirmButton = { TextButton(onClick = onConfirm) { Text(stringResource(R.string.make_archive)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

/**
 * Choosing what a profile selects, full screen: the library's albums, artists, playlists and
 * album groups under tabs, ticked if [selected]. Each tick calls [onToggle] at once.
 */
@Composable
private fun ProfileItemsDialog(
    selected: Set<ProfileItem>,
    onToggle: (ProfileItem, Boolean) -> Unit,
    onDismiss: () -> Unit,
    syncer: Syncer,
) {
    val context = LocalContext.current
    var choices by remember { mutableStateOf<ProfileChoices?>(null) }
    var error by remember { mutableStateOf<Int?>(null) }
    LaunchedEffect(Unit) {
        try {
            choices = syncer.profileChoices()
        } catch (e: MusicCoreException) {
            error = e.status
        }
    }
    var tab by rememberSaveable { mutableIntStateOf(0) }
    val kind = ProfileItemKind.entries[tab]

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false),
    ) {
        // The dialog's window draws behind the system bars, which need icons that show on it.
        val window = (LocalView.current.parent as? DialogWindowProvider)?.window
        val light = MaterialTheme.colorScheme.surface.luminance() > 0.5f
        SideEffect {
            window?.let { WindowCompat.getInsetsController(it, it.decorView) }?.apply {
                isAppearanceLightStatusBars = light
                isAppearanceLightNavigationBars = light
            }
        }
        Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
            Column(Modifier.windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal))) {
                ScreenHeader(stringResource(R.string.what_to_sync), onDismiss) {
                    TextButton(onClick = onDismiss) { Text(stringResource(R.string.done)) }
                }
                Text(
                    selectionText(selected.toList()),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp).semantics { liveRegion = LiveRegionMode.Polite },
                )
                PrimaryScrollableTabRow(selectedTabIndex = tab, edgePadding = 16.dp) {
                    ProfileItemKind.entries.forEachIndexed { i, k ->
                        Tab(selected = tab == i, onClick = { tab = i }, text = { Text(kindTitle(k)) })
                    }
                }
                val list = choices?.let { kind.choicesIn(it) }
                Box(Modifier.fillMaxSize()) {
                    when {
                        error != null -> Note(stringResource(R.string.list_failed, statusText(context, error!!)))
                        list == null -> CircularProgressIndicator(Modifier.align(Alignment.Center))
                        list.isEmpty() -> Note(stringResource(kind.emptyText))
                        else -> LazyColumn(
                            contentPadding = WindowInsets.navigationBars.union(WindowInsets(bottom = 8.dp)).asPaddingValues(),
                            modifier = Modifier.fillMaxSize(),
                        ) {
                            items(list, key = { it.uid }) { choice ->
                                val item = ProfileItem(kind, choice.uid)
                                val checked = item in selected
                                ListItem(
                                    leadingContent = { Checkbox(checked = checked, onCheckedChange = null) },
                                    headlineContent = { Text(choice.name) },
                                    supportingContent = choiceDetail(choice)?.let { detail -> { Text(detail) } },
                                    modifier = Modifier.toggleable(checked, role = Role.Checkbox) { onToggle(item, it) },
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}

private fun ProfileItemKind.choicesIn(choices: ProfileChoices): List<ProfileChoice> = when (this) {
    ProfileItemKind.Album -> choices.albums
    ProfileItemKind.Artist -> choices.artists
    ProfileItemKind.Playlist -> choices.playlists
    ProfileItemKind.AlbumGroup -> choices.albumGroups
}

private val ProfileItemKind.emptyText: Int
    get() = when (this) {
        ProfileItemKind.Album -> R.string.no_albums
        ProfileItemKind.Artist -> R.string.no_artists
        ProfileItemKind.Playlist -> R.string.no_playlists
        ProfileItemKind.AlbumGroup -> R.string.no_album_groups
    }

@Composable
private fun kindTitle(kind: ProfileItemKind): String = stringResource(
    when (kind) {
        ProfileItemKind.Album -> R.string.section_albums
        ProfileItemKind.Artist -> R.string.section_artists
        ProfileItemKind.Playlist -> R.string.section_playlists
        ProfileItemKind.AlbumGroup -> R.string.section_album_groups
    },
)

// An album's artist and year; an artist's or album group's album count.
@Composable
private fun choiceDetail(choice: ProfileChoice): String? = when {
    choice.artist != null && choice.year > 0 -> stringResource(R.string.album_byline_year, choice.artist, choice.year)
    choice.artist != null -> choice.artist
    choice.albumCount > 0 -> pluralStringResource(R.plurals.count_albums, choice.albumCount, choice.albumCount)
    else -> null
}

/** "3 albums, 1 playlist selected", or that nothing is. */
@Composable
private fun selectionText(items: List<ProfileItem>): String {
    val counts = ProfileItemKind.entries.mapNotNull { kind ->
        val n = items.count { it.kind == kind }
        if (n == 0) return@mapNotNull null
        val plural = when (kind) {
            ProfileItemKind.Album -> R.plurals.count_albums
            ProfileItemKind.Artist -> R.plurals.count_artists
            ProfileItemKind.Playlist -> R.plurals.count_playlists
            ProfileItemKind.AlbumGroup -> R.plurals.count_album_groups
        }
        pluralStringResource(plural, n, n)
    }
    return if (counts.isEmpty()) stringResource(R.string.nothing_selected)
    else stringResource(R.string.count_selected, counts.joinToString(stringResource(R.string.import_list_separator)))
}

/** "Original files" for 0 kbps, else "Opus 128 kbps". */
@Composable
fun formatText(kbps: Int): String =
    if (kbps == 0) stringResource(R.string.format_original) else stringResource(R.string.format_opus, kbps)

// A row's text greyed out with its control while it can't be changed.
@Composable
private fun itemColors(enabled: Boolean): ListItemColors {
    if (enabled) return ListItemDefaults.colors()
    val disabled = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.38f)
    return ListItemDefaults.colors(headlineColor = disabled, supportingColor = disabled, trailingIconColor = disabled)
}

@Composable
private fun SubHeading(title: String) {
    Text(
        title,
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = 4.dp),
    )
}

@Composable
private fun SecondaryText(text: String) {
    Text(text, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
}

// A line of explanation in place of something that can't be shown; nothing for null.
@Composable
private fun Note(text: String?) {
    if (text == null) return
    Text(
        text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.fillMaxWidth().padding(16.dp),
    )
}

/**
 * [bytes] as "12.5 MB", in units of 1024 as the desktop apps show sizes and as budgets are
 * set in GB, so a 32 GB budget reads as 32 GB everywhere (Android's own formatter counts in
 * 1000s).
 */
fun sizeText(context: Context, bytes: Long): String {
    var value = bytes.toDouble()
    var unit = 0
    while (value >= 1024 && unit < SIZE_UNITS.size) {
        value /= 1024
        unit++
    }
    if (unit == 0) return context.resources.getQuantityString(R.plurals.size_bytes, bytes.toInt(), bytes.toInt())
    val digits = if (value < 100) 1 else 0
    val number = NumberFormat.getNumberInstance().apply {
        minimumFractionDigits = digits
        maximumFractionDigits = digits
    }
    return context.getString(SIZE_UNITS[unit - 1], number.format(value))
}

private val SIZE_UNITS = listOf(R.string.size_kb, R.string.size_mb, R.string.size_gb, R.string.size_tb)

/** The reason for [status], a negative mc_status, as the core words it (status_*). */
fun statusText(context: Context, status: Int): String = context.getString(
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

private const val APPLY_DELAY_MS = 400L
private const val ESTIMATE_DELAY_MS = 300L
private const val DEFAULT_BUDGET_GB = 32L
private const val GIB = 1024L * 1024 * 1024
