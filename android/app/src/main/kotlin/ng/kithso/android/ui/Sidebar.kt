package ng.kithso.android.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
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
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshots.Snapshot
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

/** The library pages of the tablet and rail layouts, which the sidebar or the rail picks between. */
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

/** A section heading's: Recents'. */
private val SidebarHeadingHeight = 36.dp

/** The space above the first row, between the groups of pages, and below the last. */
private val SidebarGap = 8.dp

/** The fewest recents the sidebar lists, however little room there is. */
internal const val MIN_SIDEBAR_RECENTS = 1

/** How tall a row is with the text at [fontScale]: taller only once the text needs it. */
private fun sidebarRowHeight(fontScale: Float): Dp = maxOf(SidebarRowHeight, 24.dp * fontScale + 16.dp)

/**
 * How tall the sidebar has to be to show, without scrolling, its pages and, beneath the
 * divider, Library stats, Sync, Import and Settings. Recents go between them and scroll.
 * [windowLayout] wants this much room under the top bar for the tablet layout.
 */
internal fun sidebarFixedHeight(fontScale: Float): Dp {
    val row = sidebarRowHeight(fontScale)
    val pages = 2 + TabletSection.entries.size - 1 // Home and Search; Albums to Album groups
    val pinned = 4 // Library stats, Sync, Import and Settings
    return SidebarGap + row * pages + SidebarGap + 1.dp + SidebarGap + row * pinned + SidebarGap
}

/**
 * How many of [available] recents the sidebar lists in [height], the room between its pages
 * and the divider: as many as fit there without scrolling, but at least [MIN_SIDEBAR_RECENTS].
 */
internal fun sidebarRecentsShown(height: Dp, fontScale: Float, available: Int): Int {
    val fit = ((height - SidebarHeadingHeight) / sidebarRowHeight(fontScale)).toInt()
    return minOf(available, maxOf(MIN_SIDEBAR_RECENTS, fit))
}

/**
 * The tablet layout's sidebar, as the desktops'. Library stats, Sync, Import and Settings stay
 * at the bottom; what's above scrolls, and Recents lists as many as fit ([sidebarRecentsShown]).
 * The picked entry is [page] if the sidebar opened one, else [section]. [binDot] marks Sync
 * while deletes wait for a sync, as the phone's menu does, and so does a lost archive this
 * tablet may replace. The sync pill is pinned below, but on the Sync page, which has its own.
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
    modifier: Modifier = Modifier,
) {
    val row = sidebarRowHeight(LocalDensity.current.fontScale)
    val status by syncer.status.collectAsState()
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
            BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
                val shown = recents.take(sidebarRecentsShown(maxHeight, LocalDensity.current.fontScale, available = recents.size))
                val list = rememberLazyListState()
                // A recent arriving at the top shows there, while the list is at its top. The list
                // would otherwise stay with the item that was first, scrolling the new one out of
                // sight. This has to be before the list's next layout, which is when it follows.
                val front = shown.firstOrNull()?.ref
                remember(front) {
                    Snapshot.withoutReadObservation {
                        if (list.firstVisibleItemIndex == 0 && list.firstVisibleItemScrollOffset == 0) list.requestScrollToItem(0)
                    }
                    front
                }
                LazyColumn(Modifier.fillMaxSize(), state = list) {
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
                }
            }
            HorizontalDivider()
            Spacer(Modifier.height(SidebarGap))
            SidebarRow(stringResource(R.string.stats_title), page == Page.Stats, row, icon = Icons.BarChart) { onPage(Page.Stats) }
            SidebarRow(
                stringResource(R.string.sync),
                page == Page.Sync,
                row,
                icon = Icons.Sync,
                badge = binDot || status.offer?.asked == true,
                badgeDescription = stringResource(if (binDot) R.string.sync_bin_waiting else R.string.sync_offer_title_tablet),
            ) { onPage(Page.Sync) }
            ImportRow(importPickers, importRunning, row)
            SidebarRow(stringResource(R.string.settings), page == Page.Settings, row, icon = Icons.Settings) { onPage(Page.Settings) }
            Spacer(Modifier.height(SidebarGap))
            // Pinned below the rest: what the next sync would copy, and Sync now.
            if (status.inGroup && page != Page.Sync) {
                Surface(
                    shape = RoundedCornerShape(20.dp),
                    color = MaterialTheme.colorScheme.surfaceContainerHigh,
                    modifier = Modifier.fillMaxWidth().padding(bottom = SidebarGap),
                ) {
                    val showIssues = rememberSyncIssues(syncer)
                    SyncPillContent(
                        status, onSyncNow = { syncer.syncNow() }, onCancel = { syncer.cancelSync() },
                        onDetails = showIssues, nothingToDo = syncer.nothingToDo,
                    )
                }
            }
        }
    }
}

/** Import, which opens its choices in a menu, as the phone's + Import does. */
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

/** A heading in the sidebar. */
@Composable
private fun SidebarHeading(title: String) {
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
            .menuClickable(onLongClick, onClick)
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
                color = if (selected) colors.onSecondaryContainer else colors.onSurface,
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
