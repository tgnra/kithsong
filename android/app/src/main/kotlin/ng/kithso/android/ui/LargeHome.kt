package ng.kithso.android.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.Library

// Home in a window with three panes, as on the desktops: the categories on the left, and the one
// chosen as the middle pane, which is a MainScreen of its own kind (HomeColumn.Category). Pages
// open in the right pane.

/** The library's sections with something in them, in the order of [homePages]. */
internal fun categories(library: Library): List<Section> =
    homePages(library).filterIsInstance<HomePage.Category>().map { it.section }

/** What the middle pane shows: [chosen], while it has something in it, else Albums, else the first there is. */
internal fun shownCategory(library: Library?, chosen: Section?): Section {
    if (library == null) return chosen ?: Section.Albums
    val there = categories(library)
    return chosen?.takeIf { it in there }
        ?: Section.Albums.takeIf { it in there }
        ?: there.firstOrNull()
        ?: Section.Albums
}

/** The middle pane's list: [section] alone, its tiles in a grid [columns] wide. */
internal fun categoryItems(library: Library?, section: Section, columns: Int, banner: Boolean): List<MainItem> = buildList {
    add(MainItem.Top)
    if (banner) add(MainItem.Banner)
    if (library == null) return@buildList
    if (library.isEmpty) add(MainItem.Empty)
    if (section !in categories(library)) return@buildList
    add(MainItem.Heading(section))
    when (section) {
        Section.Radio -> library.stations.chunked(columns).forEachIndexed { row, stations -> add(MainItem.RadioRow(stations, row)) }
        Section.Recents -> library.recents.chunked(columns).forEachIndexed { row, recents -> add(MainItem.RecentsRow(recents, row)) }
        Section.Playlists -> library.playlists.mapTo(this, MainItem::PlaylistItem)
        Section.AlbumGroups -> library.albumGroups.chunked(columns).forEachIndexed { row, groups -> add(MainItem.AlbumGroupsRow(groups, row)) }
        Section.Albums -> library.albums.chunked(columns).mapTo(this, MainItem::AlbumRow)
        Section.Artists -> library.artists.mapTo(this, MainItem::ArtistItem)
    }
}

private val Section.icon: ImageVector
    get() = when (this) {
        Section.Radio -> Icons.Radio
        Section.Recents -> Icons.History
        Section.Playlists -> Icons.PlaylistPlay
        Section.AlbumGroups -> Icons.Folder
        Section.Albums -> Icons.Album
        Section.Artists -> Icons.Person
    }

/**
 * What the pages' pane shows beneath its pages if [shown], which it is with three panes, as there's
 * nothing else there. Always called, so the pages after it keep their place in the composition.
 */
@Composable
internal fun NoPages(shown: Boolean) {
    if (shown) QuietPane()
}

private val PanelHeaderHeight = 56.dp

/**
 * The left pane: the wordmark and the categories, with [chosen] marked, then the utilities at the
 * bottom, as on the desktops. It all scrolls where a window is too short for it. As on [SmallHome]
 * a list there's none of yet has a row for making the first; what's chosen and what opens are
 * [App]'s, so that they outlive this pane in a window without one.
 */
@Composable
internal fun CategoriesPanel(
    library: Library?,
    chosen: Section,
    onChoose: (Section) -> Unit,
    onSearch: () -> Unit,
    onSync: () -> Unit,
    onSettings: () -> Unit,
    onStats: () -> Unit,
    onNewList: ((isAlbums: Boolean) -> Unit)?,
    importPickers: ImportPickers,
    importRunning: Boolean,
    syncDot: Boolean,
    modifier: Modifier = Modifier,
) {
    BoxWithConstraints(modifier.fillMaxSize()) {
        Column(
            Modifier
                .verticalScroll(rememberScrollState())
                .heightIn(min = maxHeight)
                .windowInsetsPadding(WindowInsets.safeDrawing.inPane()),
            verticalArrangement = Arrangement.SpaceBetween,
        ) {
            Column(Modifier.fillMaxWidth()) {
                PanelMark()
                if (library != null) {
                    for (section in categories(library)) {
                        UtilityRow(
                            stringResource(section.title),
                            { Icon(section.icon, contentDescription = null) },
                            onClick = { onChoose(section) },
                            panel = true,
                            selected = section == chosen,
                        )
                    }
                    NewListRows(library, onNewList, panel = true)
                }
            }
            Column(Modifier.fillMaxWidth()) {
                HorizontalDivider(Modifier.padding(horizontal = 12.dp, vertical = 4.dp))
                UtilityRows(onSearch, onSync, onSettings, onStats, importPickers, importRunning, syncDot, panel = true)
            }
        }
    }
}

/** As much of the wordmark as the panel has room for. */
@Composable
private fun PanelMark() {
    BoxWithConstraints(Modifier.fillMaxWidth().height(PanelHeaderHeight).padding(horizontal = 12.dp), contentAlignment = Alignment.CenterStart) {
        val name = stringResource(R.string.app_name)
        val mark = Modifier.offset(x = (-4).dp).semantics { heading() }
        when (val fits = headerMark(maxWidth)) {
            is HeaderMark.Wordmark -> Wordmark(fits.height, name, mark)
            is HeaderMark.Tile -> WordmarkTile(fits.size, name, mark)
            HeaderMark.None -> {}
        }
    }
}
