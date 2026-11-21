package ng.kithso.android.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.plus
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusDirection
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Library
import ng.kithso.android.data.LibrarySource

/**
 * The narrowest window, less its system bars and cutouts at the sides, for the rail layout:
 * Material's medium width class, a 7" tablet in portrait.
 */
internal val RailMinWidth = 600.dp

/** How wide the rail is, less the inset at its side: Material's. */
internal val RailWidth = 80.dp

/** The widest a rail item's label gets: the rail less a gap at each side. */
private val RailLabelWidth = 68.dp

// Material's rail: the gap between its items, and above and below them, and under its header.
private val RailGap = 4.dp
private val RailHeaderGap = 8.dp

/** The menu button at the rail's top. */
private val RailMenuHeight = 48.dp

/** The focus ring's shape on a rail item. */
private val RailItemShape = RoundedCornerShape(16.dp)

/** How tall a rail item is with its label on one line at [fontScale]: Material's 32dp indicator, a 4dp gap, the label's 16sp and 4dp above and below. */
private fun railItemHeight(fontScale: Float): Dp = 44.dp + 16.dp * fontScale

/**
 * How tall the rail has to be to show, without scrolling, its menu, the library pages and
 * Search, then Sync and Settings. [windowLayout] wants this much room for the rail layout.
 */
internal fun railHeight(fontScale: Float): Dp {
    val items = TabletSection.entries.size + 3 // the library pages, Search, Sync and Settings
    // Above the menu, either side of its gap, between the items and below the last.
    val gaps = RailGap * (items + 3)
    return RailMenuHeight + RailHeaderGap + railItemHeight(fontScale) * items + gaps
}

/**
 * The layout for a window between the phone's and the tablet layout's, a 7" tablet in portrait
 * say: a rail down the side for the library pages, Search, Sync and Settings, and beside it the
 * tablet layout's content ([TabletContent]) without its pane, so a track list opens over the
 * page as on the phone. The rail's too narrow to list the devices: Sync opens the phone's Sync
 * page. It shares the phone's back stack, as [TabletLayout] does, and the players, editors,
 * menus and dialogs over it are [App]'s.
 *
 * [dock] is the phone's mini player or sync pill, if it's showing, which goes at the bottom of
 * the content, clear of the rail; [bottomPadding] keeps pages' ends clear of it. There's no
 * drag and drop here: long presses open menus, as on the phone. [covered]: the player or an
 * editor is over this, which then has neither back nor TalkBack.
 *
 * The keyboard's focus goes round as in the tablet layout ([TabletFocus]): Tab from the rail to
 * the content, then the dock, then the rail again.
 */
@Composable
internal fun RailLayout(
    nav: BackStack<Page>,
    updateNav: ((BackStack<Page>) -> BackStack<Page>) -> Unit,
    section: TabletSection,
    onSection: (TabletSection) -> Unit,
    shape: WindowShape,
    library: Library?,
    source: LibrarySource,
    actions: PlaylistActions,
    importPickers: ImportPickers,
    importRunning: Boolean,
    binDot: Boolean,
    offerDot: Boolean,
    banner: (@Composable (Modifier) -> Unit)?,
    covered: Boolean,
    bottomPadding: Dp,
    onOpen: (CollectionRef) -> Unit,
    pageContent: PageContent,
    dock: (@Composable (Modifier) -> Unit)?,
    modifier: Modifier = Modifier,
) {
    val rtl = LocalLayoutDirection.current == LayoutDirection.Rtl
    val open = nav.layers.filter { it.page != Page.Player }
    val root = open.firstOrNull()?.takeIf { it.page.isSidebarPage() }
    val top = nav.top
    val focus = rememberTabletFocus(covered)
    SideEffect { focus.end = if (dock != null) focus.dock else null }
    val clear = { stack: BackStack<Page> -> stack.removeAll { it != Page.Player } }
    val openFromBase: (CollectionRef) -> Unit = { ref ->
        updateNav { stack -> stack.removeAll { it is Page.Collection } }
        onOpen(ref)
    }

    Box(modifier.fillMaxSize().focusArea(focus.window, covered = covered)) {
        Row(Modifier.fillMaxSize()) {
            CompositionLocalProvider(LocalTabExit provides focus.sidebarExit) {
                Rail(
                    section,
                    root?.page,
                    binDot,
                    offerDot,
                    importPickers,
                    importRunning,
                    onSection = { picked ->
                        onSection(picked)
                        updateNav(clear)
                    },
                    // Picked again, it stays as it is, with what's typed in Search.
                    onPage = { page -> if (root?.page != page || open.size > 1) updateNav { clear(it).push(page) } },
                    modifier = Modifier.coveredIf(covered).focusArea(focus.sidebar),
                )
            }
            // Pages go under the status bar, as on the phone, and keep their content clear of it.
            TabletContent(
                updateNav, open, root, top, section, shape.fold, rtl, bottomPadding, covered, pane = false,
                pageContent, openFromBase, onOpen, banner, library, source, actions, importPickers, focus,
                Modifier.weight(1f).fillMaxHeight().coveredIf(covered),
                pageTop = WindowInsets.statusBars,
            )
        }
        dock?.invoke(
            Modifier
                .align(Alignment.BottomCenter)
                .padding(start = RailWidth)
                // Tab past its last button goes to the rail, and Shift+Tab before its first to
                // the content, where the focus was there.
                .focusProperties {
                    onExit = {
                        when (requestedFocusDirection) {
                            FocusDirection.Next -> focus.sidebar.focusIfThere()
                            FocusDirection.Previous -> focus.focusContent()
                        }
                    }
                }
                .focusArea(focus.dock, covered = covered),
        )
    }
}

/**
 * The rail: the menu, with Import and Library stats, then Home, Search and the library pages,
 * which scroll if the labels need more room than [railHeight] counts, and Sync and Settings at
 * the bottom. The picked item is [page]'s if the rail opened one, Sync's for a device's page,
 * else [section]'s. [binDot] marks Sync while deletes wait for a sync, as the phone's menu does, and [offerDot]
 * while this tablet may take over a lost archive, as the sidebar's This tablet does.
 */
@Composable
private fun Rail(
    section: TabletSection,
    page: Page?,
    binDot: Boolean,
    offerDot: Boolean,
    importPickers: ImportPickers,
    importRunning: Boolean,
    onSection: (TabletSection) -> Unit,
    onPage: (Page) -> Unit,
    modifier: Modifier = Modifier,
) {
    NavigationRail(
        modifier = modifier,
        containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        header = { RailMenu(importPickers, importRunning, onStats = { onPage(Page.Stats) }) },
        windowInsets = WindowInsets.systemBars.union(WindowInsets.displayCutout).only(WindowInsetsSides.Vertical + WindowInsetsSides.Start),
    ) {
        Column(
            Modifier.weight(1f).verticalScroll(rememberScrollState()),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(RailGap),
        ) {
            val pick: @Composable (TabletSection) -> Unit = { s ->
                RailItem(s.title, s.icon, page == null && section == s) { onSection(s) }
            }
            pick(TabletSection.Home)
            RailItem(R.string.search, Icons.Search, page == Page.Search) { onPage(Page.Search) }
            for (s in TabletSection.entries) if (s != TabletSection.Home) pick(s)
        }
        RailItem(
            R.string.sync,
            Icons.Sync,
            page == Page.Sync || page is Page.Device,
            badge = binDot || offerDot,
            badgeDescription = stringResource(if (binDot) R.string.sync_bin_waiting else R.string.sync_offer_title_tablet),
        ) { onPage(Page.Sync) }
        RailItem(R.string.settings, Icons.Settings, page == Page.Settings) { onPage(Page.Settings) }
    }
}

/** One of the rail's items, with [badge]'s dot on its icon, which [badgeDescription] says. A list's item to the keyboard ([listItemKeys]). */
@Composable
private fun RailItem(
    @StringRes label: Int,
    icon: ImageVector,
    selected: Boolean,
    badge: Boolean = false,
    badgeDescription: String? = null,
    onClick: () -> Unit,
) {
    NavigationRailItem(
        selected = selected,
        onClick = onClick,
        icon = {
            BadgedBox(badge = { if (badge) Badge() }) {
                Icon(icon, contentDescription = if (badge) badgeDescription else null)
            }
        },
        label = {
            // Kept inside the rail: a long label ("Album groups") wraps rather than reaching its edges.
            Text(
                stringResource(label),
                textAlign = TextAlign.Center,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.widthIn(max = RailLabelWidth),
            )
        },
        modifier = Modifier.listItemKeys(RailItemShape, null),
    )
}

/**
 * The rail's menu: what the sidebar has beneath its divider but Settings. Import's choices take
 * the menu's place, with a way back, and while an import runs Import has a spinner, as in the
 * phone's main menu.
 */
@Composable
private fun RailMenu(importPickers: ImportPickers, importRunning: Boolean, onStats: () -> Unit) {
    var open by remember { mutableStateOf(false) }
    var importing by remember { mutableStateOf(false) }
    Box {
        IconButton(
            onClick = {
                importing = false
                open = true
            },
        ) {
            Icon(Icons.MoreHoriz, contentDescription = stringResource(R.string.main_menu))
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            if (importing) {
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.back)) },
                    leadingIcon = { Icon(Icons.ArrowBack, contentDescription = null) },
                    onClick = { importing = false },
                )
                ImportChoices(importPickers, onDismiss = { open = false })
            } else {
                val runningText = stringResource(R.string.import_running_title)
                DropdownMenuItem(
                    text = {
                        Text(
                            stringResource(R.string.import_music),
                            Modifier.semantics { if (importRunning) contentDescription = runningText },
                        )
                    },
                    leadingIcon = { Icon(Icons.Import, contentDescription = null) },
                    trailingIcon = if (importRunning) {
                        { CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp) }
                    } else {
                        null
                    },
                    onClick = { importing = true },
                )
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.stats_title)) },
                    leadingIcon = { Icon(Icons.BarChart, contentDescription = null) },
                    onClick = {
                        open = false
                        onStats()
                    },
                )
            }
        }
    }
}
