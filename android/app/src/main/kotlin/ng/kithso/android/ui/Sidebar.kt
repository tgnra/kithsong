package ng.kithso.android.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Recent
import ng.kithso.android.sync.Syncer
import ng.kithso.android.ui.art.AlbumArt

/** The tablet layout's library pages, which the sidebar picks between. */
enum class TabletSection(@StringRes val title: Int, val icon: ImageVector) {
    Home(R.string.section_home, Icons.Home),
    Albums(R.string.section_albums, Icons.Album),
    Artists(R.string.section_artists, Icons.Person),
    Playlists(R.string.section_playlists, Icons.PlaylistPlay),
    AlbumGroups(R.string.section_album_groups, Icons.LibraryMusic),
}

/** How wide the sidebar is, unless it reaches to a fold (see [tabletSidebarWidth]). */
internal val SidebarWidth = 240.dp

/** A row of the sidebar at the usual text size: a little under a list's, so it all fits a tablet held sideways. */
private val SidebarRowHeight = 44.dp

/** A section heading's, Recents' and Devices'. */
private val SidebarHeadingHeight = 36.dp

/** The space above the first row, between the groups of pages, and below the last. */
private val SidebarGap = 8.dp

/** How many of Recents count towards the sidebar's height, which the rest scroll past. */
private const val FIXED_RECENTS = 3

/** How many recents the sidebar lists, as on the desktops. */
private const val MAX_RECENTS = 10

/** How tall a row is with the text at [fontScale]: taller only once the text needs it. */
private fun sidebarRowHeight(fontScale: Float): Dp = maxOf(SidebarRowHeight, 24.dp * fontScale + 16.dp)

/**
 * How tall the sidebar has to be to show, without scrolling, its pages, the Recents heading
 * and the first [FIXED_RECENTS] recents, with Import and Settings beneath them. Devices and the
 * rest of Recents can scroll. [useTabletLayout] wants this much room under the top bar.
 */
internal fun sidebarFixedHeight(fontScale: Float): Dp {
    val row = sidebarRowHeight(fontScale)
    val pages = 2 + TabletSection.entries.size - 1 // Home and Search; Albums to Album groups
    val pinned = 2 // Import and Settings
    return SidebarGap + row * pages + SidebarGap + SidebarHeadingHeight + row * FIXED_RECENTS +
        1.dp + SidebarGap + row * pinned + SidebarGap
}

/**
 * The tablet layout's sidebar, as the desktops': Home and Search, then the library's pages,
 * then Recents and Devices, scrolling between those and Import and Settings, which stay at
 * the bottom. What's showing is [section], unless a page the sidebar opens is ([page]:
 * Search, Settings, Library stats or a device's page), which is then the one picked.
 *
 * A recent opens with [onRecent], and long-pressing or right-clicking it opens its menu
 * ([onRecentMenu]). A track dragged onto a manual playlist's, or an album onto a manual album
 * group's, is added to it ([DragDrop]).
 * Devices lists this phone, the devices it syncs with and those on the network it could pair
 * with, as the phone's Sync page does: a device opens its page ([onDevice], [THIS_DEVICE] for
 * this phone), a nearby one asks to pair, and the + adds one by address. [binDot] marks this
 * phone while deletes wait for a sync it would send, as the phone's menu marks Sync. Import
 * opens [importPickers]' choices in a menu, with a spinner while an import is
 * [importRunning]. Library stats, which the desktops have at the bottom, goes at the end of
 * what scrolls, as there's no room for it among what doesn't.
 */
@Composable
internal fun Sidebar(
    section: TabletSection,
    page: Page?,
    recents: List<Recent>,
    syncer: Syncer,
    binDot: Boolean,
    importPickers: ImportPickers,
    importRunning: Boolean,
    onSection: (TabletSection) -> Unit,
    onPage: (Page) -> Unit,
    onRecent: (Recent) -> Unit,
    onRecentMenu: (Recent) -> Unit,
    onDevice: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    val row = sidebarRowHeight(LocalDensity.current.fontScale)
    val status by syncer.status.collectAsState()
    var adding by rememberSaveable { mutableStateOf(false) }
    Surface(color = MaterialTheme.colorScheme.surfaceContainerLow, modifier = modifier.fillMaxHeight()) {
        Column(
            Modifier
                .windowInsetsPadding(
                    WindowInsets.safeDrawing.only(WindowInsetsSides.Start)
                        .union(WindowInsets.navigationBars.only(WindowInsetsSides.Bottom)),
                )
                .padding(horizontal = 12.dp),
        ) {
            Spacer(Modifier.height(SidebarGap))
            val pick: @Composable (TabletSection) -> Unit = { s ->
                SidebarRow(stringResource(s.title), page == null && section == s, row, icon = s.icon) { onSection(s) }
            }
            pick(TabletSection.Home)
            SidebarRow(stringResource(R.string.search), page == Page.Search, row, icon = Icons.Search) { onPage(Page.Search) }
            Spacer(Modifier.height(SidebarGap))
            for (s in TabletSection.entries) if (s != TabletSection.Home) pick(s)
            Spacer(Modifier.height(SidebarGap))
            LazyColumn(Modifier.weight(1f).fillMaxWidth()) {
                val shown = recents.take(MAX_RECENTS)
                if (shown.isNotEmpty()) {
                    item(key = "recents") { SidebarHeading(stringResource(R.string.section_recents)) }
                    items(shown, key = { "recent:${it.ref.kind}:${it.ref.uid}" }) { recent ->
                        SidebarRow(
                            recent.title,
                            selected = false,
                            height = row,
                            leading = {
                                AlbumArt(
                                    recent.art,
                                    contentDescription = null,
                                    shape = if (recent.ref.kind == CollectionRef.Kind.Artist) CircleShape else RoundedCornerShape(6.dp),
                                    modifier = Modifier.size(28.dp),
                                )
                            },
                            onLongClick = { onRecentMenu(recent) },
                            dropOn = recent.ref,
                        ) { onRecent(recent) }
                    }
                }
                item(key = "devices") {
                    SidebarHeading(stringResource(R.string.devices)) {
                        IconButton(onClick = { adding = true }, enabled = status.running && status.pairing == null) {
                            Icon(Icons.Add, contentDescription = stringResource(R.string.add_device))
                        }
                    }
                }
                item(key = "self") {
                    SidebarRow(
                        status.deviceName,
                        selected = page == Page.Device(THIS_DEVICE),
                        height = row,
                        icon = Icons.Smartphone,
                        subtitle = stringResource(R.string.this_phone),
                        badge = binDot,
                        badgeDescription = stringResource(R.string.sync_bin_waiting),
                    ) { onDevice(THIS_DEVICE) }
                }
                items(status.members, key = { "member:${it.uid}" }) { device ->
                    val syncing = device.uid in status.syncing
                    SidebarRow(
                        device.name,
                        selected = page == Page.Device(device.uid),
                        height = row,
                        icon = device.kind.icon,
                        // Its first line: how it is, without why files wait, which its page says.
                        subtitle = memberText(device, syncing).substringBefore('\n'),
                        trailing = if (syncing) ({ CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp) }) else null,
                    ) { onDevice(device.uid) }
                }
                if (status.nearby.isNotEmpty()) item(key = "nearby") { SidebarHeading(stringResource(R.string.devices_nearby)) }
                items(status.nearby, key = { "nearby:${it.uid}" }) { device ->
                    SidebarRow(
                        device.name,
                        selected = false,
                        height = row,
                        icon = device.kind.icon,
                        subtitle = stringResource(if (device.inOtherGroup) R.string.device_in_other_group else R.string.device_tap_to_pair),
                        enabled = !device.inOtherGroup && status.pairing == null,
                        clickLabel = stringResource(R.string.pair),
                    ) { syncer.pair(device) }
                }
                item(key = "stats") {
                    Spacer(Modifier.height(SidebarGap))
                    SidebarRow(stringResource(R.string.stats_title), page == Page.Stats, row, icon = Icons.BarChart) { onPage(Page.Stats) }
                    Spacer(Modifier.height(SidebarGap))
                }
            }
            HorizontalDivider()
            Spacer(Modifier.height(SidebarGap))
            ImportRow(importPickers, importRunning, row)
            SidebarRow(stringResource(R.string.settings), page == Page.Settings, row, icon = Icons.Settings) { onPage(Page.Settings) }
            Spacer(Modifier.height(SidebarGap))
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
}

/** Import, which opens its choices in a menu, as the phone's main menu does. */
@Composable
private fun ImportRow(importPickers: ImportPickers, importRunning: Boolean, height: Dp) {
    var open by rememberSaveable { mutableStateOf(false) }
    Box {
        SidebarRow(
            stringResource(R.string.import_music),
            selected = false,
            height = height,
            icon = Icons.Import,
            subtitle = if (importRunning) stringResource(R.string.import_running_title) else null,
            trailing = if (importRunning) ({ CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp) }) else null,
        ) { open = true }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            ImportChoices(importPickers, onDismiss = { open = false })
        }
    }
}

/** A heading in the sidebar, with [action], if any, at its end. */
@Composable
private fun SidebarHeading(title: String, action: (@Composable () -> Unit)? = null) {
    Row(
        Modifier.fillMaxWidth().heightIn(min = SidebarHeadingHeight).padding(start = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            title,
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.weight(1f).semantics { heading() },
        )
        action?.invoke()
    }
}

/**
 * One row of the sidebar, as a navigation drawer's: [icon] or [leading], the [title] and maybe
 * a [subtitle], and [trailing] at the end, filled in while [selected]. TalkBack hears it as a
 * tab, picked or not. [badge] puts a dot on the icon, which [badgeDescription] says. A track
 * or album dropped on it is added to [dropOn], if that's a manual playlist or album group.
 */
@Composable
private fun SidebarRow(
    title: String,
    selected: Boolean,
    height: Dp,
    icon: ImageVector? = null,
    leading: (@Composable () -> Unit)? = null,
    subtitle: String? = null,
    trailing: (@Composable () -> Unit)? = null,
    badge: Boolean = false,
    badgeDescription: String? = null,
    enabled: Boolean = true,
    clickLabel: String? = null,
    onLongClick: (() -> Unit)? = null,
    dropOn: CollectionRef? = null,
    onClick: () -> Unit,
) {
    val colors = MaterialTheme.colorScheme
    val shape = RoundedCornerShape(height / 2)
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = height)
            .clip(shape)
            .then(if (selected) Modifier.background(colors.secondaryContainer) else Modifier)
            .then(if (dropOn != null) Modifier.dropTarget(dropOn, title, shape) else Modifier)
            .listItemKeys(shape, onLongClick)
            .menuClickable(onLongClick, onClick, enabled = enabled, onClickLabel = clickLabel)
            .semantics {
                this.selected = selected
                role = Role.Tab
            }
            .padding(horizontal = 12.dp, vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        val content = if (selected) colors.onSecondaryContainer else colors.onSurfaceVariant
        when {
            leading != null -> leading()
            icon != null -> BadgedBox(badge = { if (badge) Badge() }) {
                Icon(icon, contentDescription = if (badge) badgeDescription else null, tint = content)
            }
        }
        Column(Modifier.weight(1f)) {
            Text(
                title,
                style = MaterialTheme.typography.labelLarge,
                color = if (selected) colors.onSecondaryContainer else if (enabled) colors.onSurface else colors.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            if (subtitle != null) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = content,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        trailing?.invoke()
    }
}
