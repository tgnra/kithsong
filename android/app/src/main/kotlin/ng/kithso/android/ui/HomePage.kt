package ng.kithso.android.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyItemScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material3.Button
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Home
import ng.kithso.android.data.HomeRow
import ng.kithso.android.data.Library
import ng.kithso.android.data.LibrarySource
import ng.kithso.android.data.Recent
import ng.kithso.android.data.Station
import ng.kithso.android.ui.art.PrefetchArt
import ng.kithso.android.ui.theme.ArtBackdrop
import ng.kithso.android.ui.theme.rememberOnScreenArtColors

// The Home page, for the tablet layout's content pane: titled rows of covers, one under
// another, each scrolling sideways on its own, as on the desktops.
//
//     Radio
//     +------+ +------+ +------+ +------+ +---
//     | art  | | art  | | art  | | art  | |       <- scrolls sideways
//     +------+ +------+ +------+ +------+ +---
//     Recents
//     +------+ +------+ +------+ +------+ +---
//     ...
//
// What's in each row is the core's business (kithcore/home_picks.h, by way of
// LibrarySource.home); this only shows it, with the main screen's tiles, art and backdrop.

/**
 * The Home page: [source]'s Home rows (Radio, Recents, Played a lot recently, Not played in a
 * while, Random), each a [TileRow] under its heading, leaving out a row with nothing in it.
 *
 * Tapping a tile opens it with [onOpen], or if it's a station plays it with [onPlayStation].
 * Long-pressing one opens the menu it has elsewhere through [actions] (an album's, an artist's, a
 * playlist's, an album group's or a station's), which whatever shows this has to show with
 * [PlaylistSheets]; a tile under Recents has the main screen's recents' menu, with Remove from
 * Recents and Clear Recents.
 *
 * A library with no music shows a welcome, with Import opening [importPickers]; one with nothing
 * played yet says at the foot that the page fills up as it is. Nothing shows until the rows have
 * first been read. They're read again after a play or a change to the library, and the tiles
 * update where they are, so each row keeps its place; the source only does that while this is
 * collecting them ([LibrarySource.home]), and reads them again when it's next shown.
 *
 * [contentPadding] goes around the rows inside the scrolling list, for a top bar or mini player
 * that the list goes under; the rows have the gutter of their own at the sides.
 */
@Composable
fun HomePage(
    source: LibrarySource,
    actions: PlaylistActions,
    onOpen: (CollectionRef) -> Unit,
    onPlayStation: (CollectionRef) -> Unit,
    importPickers: ImportPickers,
    modifier: Modifier = Modifier,
    contentPadding: PaddingValues = PaddingValues(),
) {
    val library by source.library.collectAsState()
    val home by source.home.collectAsState()
    val items = remember(library, home) { homeItems(library, home) }
    val index = remember(library) { LibraryIndex(library) }
    val listState = rememberLazyListState()
    // Above the list, which drops a row's item when it scrolls away (see rememberTileRowState).
    val rowStates = HomeRow.Kind.entries.associateWith { rememberTileRowState(listState, rowKey(it)) }

    BoxWithConstraints(modifier.fillMaxSize()) {
        val tilePx = with(LocalDensity.current) { tileWidth(maxWidth).roundToPx() }
        PrefetchArt(listState, tilePx) { i -> items.getOrNull(i)?.art.orEmpty() }
        ArtBackdrop(
            rememberOnScreenArtColors(listState) { i -> items.getOrNull(i)?.art.orEmpty() },
            Modifier.matchParentSize(),
        )
        LazyColumn(state = listState, contentPadding = contentPadding, modifier = Modifier.fillMaxSize()) {
            items(items, key = { it.key }, contentType = { it.contentType }) { item ->
                when (item) {
                    HomeItem.Welcome -> Welcome(importPickers)
                    HomeItem.NoHistory -> NoHistory()
                    is HomeItem.Heading -> RowHeading(item.kind.title)
                    is HomeItem.Row -> TileRow(
                        item.row.items.map { recent -> homeTile(item.row.kind, recent, index, actions, onOpen, onPlayStation) },
                        state = rowStates.getValue(item.row.kind),
                    )
                }
            }
        }
    }
}

/** One item of the Home page's list: a row and its heading are an item each. */
@Immutable
private sealed interface HomeItem {
    val key: Any
    val contentType: Any
    val art: List<ArtRef> get() = emptyList()

    data object Welcome : HomeItem {
        override val key get() = "welcome"
        override val contentType get() = "welcome"
    }

    data object NoHistory : HomeItem {
        override val key get() = "no-history"
        override val contentType get() = "no-history"
    }

    data class Heading(val kind: HomeRow.Kind) : HomeItem {
        override val key get() = "heading:$kind"
        override val contentType get() = "heading"
    }

    data class Row(val row: HomeRow) : HomeItem {
        override val key get() = rowKey(row.kind)
        override val contentType get() = "tiles"
        override val art get() = row.items.take(ROW_ART).map { it.art }
    }
}

/** How many of a row's tiles count towards art prefetching and the backdrop: about a screen's. */
private const val ROW_ART = 6

private fun rowKey(kind: HomeRow.Kind) = "row:$kind"

// Nothing until both have been read, rather than a flash of the welcome. Either saying there's
// no music is enough for the welcome, as the rows can be a moment behind the library.
private fun homeItems(library: Library?, home: Home?): List<HomeItem> = buildList {
    if (library == null || home == null) return@buildList
    if (library.isEmpty || !home.hasMusic) {
        add(HomeItem.Welcome)
        return@buildList
    }
    for (row in home.rows) {
        add(HomeItem.Heading(row.kind))
        add(HomeItem.Row(row))
    }
    if (!home.hasPlays) add(HomeItem.NoHistory)
}

@get:StringRes
private val HomeRow.Kind.title: Int
    get() = when (this) {
        HomeRow.Kind.Radio -> R.string.home_row_radio
        HomeRow.Kind.Recents -> R.string.section_recents
        HomeRow.Kind.PlayedLately -> R.string.home_row_played_lately
        HomeRow.Kind.NotPlayedLately -> R.string.home_row_not_played_lately
        HomeRow.Kind.Random -> R.string.home_row_random
    }

/**
 * What's listed in the library by uid, for a tile's menu, which takes the album or playlist
 * itself, and for its subtitle, which says how many tracks or albums there are.
 */
private class LibraryIndex(library: Library?) {
    val albums = library?.albums.orEmpty().associateBy { it.uid }
    val artists = library?.artists.orEmpty().associateBy { it.uid }
    val playlists = library?.playlists.orEmpty().associateBy { it.uid }
    val albumGroups = library?.albumGroups.orEmpty().associateBy { it.uid }
}

/** [recent]'s tile in a row of [kind]. */
@Composable
private fun homeTile(
    kind: HomeRow.Kind,
    recent: Recent,
    index: LibraryIndex,
    actions: PlaylistActions,
    onOpen: (CollectionRef) -> Unit,
    onPlayStation: (CollectionRef) -> Unit,
): Tile {
    val ref = recent.ref
    val station = ref.kind == CollectionRef.Kind.Station
    val subtitle = recent.homeSubtitle(index)
    return Tile(
        "${ref.kind}:${ref.uid}",
        recent.title,
        subtitle,
        recent.art,
        onLongClick = { showMenu(kind, recent, index, actions) },
        badge = if (station) ({ StationBadge() }) else null,
        description = if (station) stringResource(R.string.station_label, recent.title, subtitle, recent.seedTitle.orEmpty()) else null,
    ) { if (station) onPlayStation(ref) else onOpen(ref) }
}

/**
 * What a tile shows under its title: an album's artist, a station's line, and for the rest how
 * much is in it, as the main screen's lists say. Something the library no longer lists, as the
 * rows can be a moment behind it, says what kind of thing it is, as Recents does.
 */
@Composable
private fun Recent.homeSubtitle(index: LibraryIndex): String = when (ref.kind) {
    CollectionRef.Kind.Playlist -> index.playlists[ref.uid]?.let {
        pluralStringResource(R.plurals.tracks, it.trackCount, it.trackCount)
    }
    CollectionRef.Kind.AlbumGroup -> index.albumGroups[ref.uid]?.let {
        pluralStringResource(R.plurals.albums, it.albumCount, it.albumCount)
    }
    CollectionRef.Kind.Artist -> index.artists[ref.uid]?.subtitle()
    CollectionRef.Kind.Album, CollectionRef.Kind.Station -> null
} ?: subtitle()

// The menu [recent] has elsewhere: under Recents the recents' own, with Remove from Recents and
// Clear Recents, and otherwise the album's, artist's, playlist's, group's or station's. One the
// library no longer lists has none.
private fun showMenu(kind: HomeRow.Kind, recent: Recent, index: LibraryIndex, actions: PlaylistActions) {
    val uid = recent.ref.uid
    if (kind == HomeRow.Kind.Recents) {
        actions.recentMenu(recent)
        return
    }
    when (recent.ref.kind) {
        CollectionRef.Kind.Album -> index.albums[uid]?.let { actions.albumMenu(it) }
        CollectionRef.Kind.Artist -> index.artists[uid]?.let(actions::artistMenu)
        CollectionRef.Kind.Playlist -> index.playlists[uid]?.let(actions::playlistMenu)
        CollectionRef.Kind.AlbumGroup -> index.albumGroups[uid]?.let(actions::albumGroupMenu)
        CollectionRef.Kind.Station -> actions.stationMenu(
            Station(uid, recent.title, recent.sub, recent.seedTitle.orEmpty(), recent.art, recent.artistUid),
        )
    }
}

/** A row's heading, as the main screen's sections have, without their list of the others. */
@Composable
private fun RowHeading(@StringRes title: Int) {
    Box(
        Modifier
            .padding(top = 12.dp)
            .fillMaxWidth()
            .heightIn(min = 52.dp)
            .padding(horizontal = BrowseGutter, vertical = 8.dp),
        contentAlignment = Alignment.CenterStart,
    ) {
        Text(stringResource(title), style = MaterialTheme.typography.titleLarge, modifier = Modifier.semantics { heading() })
    }
}

/** At the foot of a library with nothing played yet: what the rows are waiting for. */
@Composable
private fun NoHistory() {
    Text(
        stringResource(R.string.home_no_history),
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        textAlign = TextAlign.Center,
        modifier = Modifier.fillMaxWidth().padding(horizontal = 32.dp, vertical = 24.dp),
    )
}

/**
 * No music yet: the desktops' welcome, and Import, which offers a folder or files as the main
 * screen's does. Sync is the main screen's to offer; the tablet layout has it to hand.
 */
@Composable
private fun LazyItemScope.Welcome(importPickers: ImportPickers) {
    Column(
        Modifier.fillParentMaxWidth().fillParentMaxHeight(0.8f).padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Icon(
            Icons.LibraryMusic,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(72.dp),
        )
        Text(
            stringResource(R.string.home_welcome_title, stringResource(R.string.app_name)),
            style = MaterialTheme.typography.titleLarge,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(top = 16.dp),
        )
        Text(
            stringResource(R.string.home_welcome_text),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(top = 4.dp),
        )
        var menu by remember { mutableStateOf(false) }
        Box(Modifier.padding(top = 24.dp)) {
            Button(onClick = { menu = true }) {
                Icon(Icons.Import, contentDescription = null, modifier = Modifier.padding(end = 8.dp).size(18.dp))
                Text(stringResource(R.string.import_music))
            }
            ImportMenu(menu, importPickers, onDismiss = { menu = false })
        }
    }
}
