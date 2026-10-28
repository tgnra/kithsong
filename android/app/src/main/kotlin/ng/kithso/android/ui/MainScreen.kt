package ng.kithso.android.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyItemScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.Album
import ng.kithso.android.data.AlbumGroup
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.Artist
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Library
import ng.kithso.android.data.Playlist
import ng.kithso.android.data.Recent
import ng.kithso.android.ui.art.AlbumArt
import ng.kithso.android.ui.art.PrefetchArt
import kotlinx.coroutines.launch

enum class Section(@StringRes val title: Int) {
    Recents(R.string.section_recents),
    Playlists(R.string.section_playlists),
    AlbumGroups(R.string.section_album_groups),
    Albums(R.string.section_albums),
    Artists(R.string.section_artists),
}

/** One item of the main list. Albums come [MainItem.AlbumRow] at a time, a grid row each. */
@Immutable
private sealed interface MainItem {
    val key: Any
    val contentType: Any
    val art: List<ArtRef> get() = emptyList()

    data object Header : MainItem {
        override val key get() = "header"
        override val contentType get() = "header"
    }

    data object Empty : MainItem {
        override val key get() = "empty"
        override val contentType get() = "empty"
    }

    data class Heading(val section: Section) : MainItem {
        override val key get() = "heading:$section"
        override val contentType get() = "heading"
    }

    data class RecentItem(val recent: Recent) : MainItem {
        override val key get() = "recent:${recent.ref.kind}:${recent.ref.uid}"
        override val contentType get() = "recent"
        override val art get() = listOf(recent.art)
    }

    data class PlaylistItem(val playlist: Playlist) : MainItem {
        override val key get() = "playlist:${playlist.uid}"
        override val contentType get() = "playlist"
        override val art get() = listOf(playlist.art)
    }

    data class AlbumGroupItem(val group: AlbumGroup) : MainItem {
        override val key get() = "group:${group.uid}"
        override val contentType get() = "playlist"
        override val art get() = listOf(group.art)
    }

    data class AlbumRow(val albums: List<Album>) : MainItem {
        override val key get() = "albums:${albums.first().uid}"
        override val contentType get() = "albums"
        override val art get() = albums.map { it.art }
    }

    data class ArtistItem(val artist: Artist) : MainItem {
        override val key get() = "artist:${artist.uid}"
        override val contentType get() = "artist"
        override val art get() = listOf(artist.art)
    }
}

// Null while the library is being read: just the header, rather than a flash of the empty state.
private fun mainItems(library: Library?, columns: Int): List<MainItem> = buildList {
    add(MainItem.Header)
    if (library == null) return@buildList
    if (library.isEmpty) {
        add(MainItem.Empty)
        return@buildList
    }
    if (library.recents.isNotEmpty()) {
        add(MainItem.Heading(Section.Recents))
        library.recents.mapTo(this, MainItem::RecentItem)
    }
    if (library.playlists.isNotEmpty()) {
        add(MainItem.Heading(Section.Playlists))
        library.playlists.mapTo(this, MainItem::PlaylistItem)
    }
    if (library.albumGroups.isNotEmpty()) {
        add(MainItem.Heading(Section.AlbumGroups))
        library.albumGroups.mapTo(this, MainItem::AlbumGroupItem)
    }
    if (library.albums.isNotEmpty()) {
        add(MainItem.Heading(Section.Albums))
        library.albums.chunked(columns).mapTo(this, MainItem::AlbumRow)
    }
    if (library.artists.isNotEmpty()) {
        add(MainItem.Heading(Section.Artists))
        library.artists.mapTo(this, MainItem::ArtistItem)
    }
}

/** A section heading's place in the list, for jumping there. */
@Immutable
private data class SectionStart(val section: Section, val index: Int)

private val Gutter = 16.dp
private val CellSpacing = 12.dp
private val MinCellWidth = 150.dp
private val ListArtSize = 56.dp

/**
 * The main screen: one lazy list of a header and the library's sections. [bottomPadding]
 * keeps the last items clear of the mini player and navigation bar.
 */
@Composable
fun MainScreen(
    library: Library?,
    bottomPadding: Dp,
    onOpen: (CollectionRef) -> Unit,
    onSearch: () -> Unit,
    onSync: () -> Unit,
    onSettings: () -> Unit,
    modifier: Modifier = Modifier,
    // Long-pressing a playlist, an album or an artist opens its menu.
    onPlaylistMenu: (Playlist) -> Unit = {},
    onAlbumMenu: (Album) -> Unit = {},
    onArtistMenu: (Artist) -> Unit = {},
) {
    BoxWithConstraints(modifier.fillMaxSize()) {
        val density = LocalDensity.current
        // The list is inset from a camera cutout or navigation bar at the side, in landscape.
        val sides = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
        val layoutDirection = LocalLayoutDirection.current
        val width = maxWidth - with(density) { (sides.getLeft(density, layoutDirection) + sides.getRight(density, layoutDirection)).toDp() }
        val columns = ((width - Gutter * 2 + CellSpacing) / (MinCellWidth + CellSpacing)).toInt().coerceAtLeast(2)
        val cellPx = with(density) { ((width - Gutter * 2 - CellSpacing * (columns - 1)) / columns).roundToPx() }
        val statusBarPx = WindowInsets.statusBars.getTop(density)

        val items = remember(library, columns) { mainItems(library, columns) }
        val sections = remember(items) {
            items.mapIndexedNotNull { i, item -> (item as? MainItem.Heading)?.let { SectionStart(it.section, i) } }
        }
        val listState = rememberLazyListState()
        // The section at the top of the screen; the header counts as part of the first.
        val current by remember(sections) {
            derivedStateOf {
                (sections.lastOrNull { it.index <= listState.firstVisibleItemIndex } ?: sections.firstOrNull())?.section
            }
        }
        val scope = rememberCoroutineScope()
        // Lands the heading just below the status bar.
        val jumpTo: (Int) -> Unit = remember(listState, statusBarPx) {
            { index -> scope.launch { listState.animateScrollToItem(index, -statusBarPx) } }
        }

        PrefetchArt(listState, cellPx) { index -> items.getOrNull(index)?.art.orEmpty() }

        LazyColumn(
            state = listState,
            // The top is left to the header, so content scrolls under the status bar.
            contentPadding = sides
                .add(WindowInsets(bottom = bottomPadding))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            items(items, key = { it.key }, contentType = { it.contentType }) { item ->
                when (item) {
                    MainItem.Header -> Header(onSearch, onSync, onSettings)
                    MainItem.Empty -> EmptyLibrary()
                    is MainItem.Heading -> SectionHeading(item.section, sections, { current }, jumpTo)
                    is MainItem.RecentItem -> LibraryRow(
                        item.recent.title, item.recent.subtitle(), item.recent.art, RoundedCornerShape(8.dp),
                    ) { onOpen(item.recent.ref) }
                    is MainItem.PlaylistItem -> LibraryRow(
                        item.playlist.name,
                        pluralStringResource(R.plurals.tracks, item.playlist.trackCount, item.playlist.trackCount),
                        item.playlist.art,
                        RoundedCornerShape(8.dp),
                        onLongClick = { onPlaylistMenu(item.playlist) },
                    ) { onOpen(CollectionRef(CollectionRef.Kind.Playlist, item.playlist.uid)) }
                    is MainItem.AlbumGroupItem -> LibraryRow(
                        item.group.name,
                        pluralStringResource(R.plurals.albums, item.group.albumCount, item.group.albumCount),
                        item.group.art,
                        RoundedCornerShape(8.dp),
                    ) { onOpen(CollectionRef(CollectionRef.Kind.AlbumGroup, item.group.uid)) }
                    is MainItem.AlbumRow -> AlbumRow(item.albums, columns, onOpen, onAlbumMenu)
                    is MainItem.ArtistItem -> LibraryRow(
                        item.artist.name,
                        item.artist.subtitle(),
                        item.artist.art,
                        CircleShape,
                        onLongClick = { onArtistMenu(item.artist) },
                    ) { onOpen(CollectionRef(CollectionRef.Kind.Artist, item.artist.uid)) }
                }
            }
        }
    }
}

/** The app title, with search, sync and settings. It scrolls away with the list. */
@Composable
private fun Header(onSearch: () -> Unit, onSync: () -> Unit, onSettings: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .windowInsetsPadding(WindowInsets.statusBars)
            .padding(start = Gutter, end = 4.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            stringResource(R.string.app_name),
            style = MaterialTheme.typography.headlineMedium,
            modifier = Modifier.weight(1f).semantics { heading() },
        )
        IconButton(onClick = onSearch) { Icon(Icons.Search, contentDescription = stringResource(R.string.search)) }
        IconButton(onClick = onSync) { Icon(Icons.Sync, contentDescription = stringResource(R.string.sync)) }
        IconButton(onClick = onSettings) { Icon(Icons.Settings, contentDescription = stringResource(R.string.settings)) }
    }
}

/**
 * A section's heading. Tapping it opens a list of every section, and picking one scrolls
 * there; [current] is the section at the top of the screen, marked in the list.
 */
@Composable
private fun SectionHeading(
    section: Section,
    sections: List<SectionStart>,
    current: () -> Section?,
    onJump: (Int) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    Box(Modifier.padding(start = Gutter - 8.dp, top = 20.dp, bottom = 4.dp)) {
        Row(
            Modifier
                .clip(RoundedCornerShape(8.dp))
                .clickable(onClickLabel = stringResource(R.string.jump_to_section)) { open = true }
                .padding(horizontal = 8.dp, vertical = 6.dp)
                .semantics { heading() },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(stringResource(section.title), style = MaterialTheme.typography.titleLarge)
            Icon(Icons.ExpandMore, contentDescription = null, modifier = Modifier.padding(start = 4.dp))
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            val here = current()
            for (start in sections) {
                DropdownMenuItem(
                    text = {
                        Text(
                            stringResource(start.section.title),
                            color = if (start.section == here) MaterialTheme.colorScheme.primary else Color.Unspecified,
                            fontWeight = if (start.section == here) FontWeight.Bold else null,
                        )
                    },
                    onClick = {
                        open = false
                        onJump(start.index)
                    },
                )
            }
        }
    }
}

/** What a recent shows under its title: an album's artist, or what kind of thing it is. */
@Composable
private fun Recent.subtitle(): String = artist ?: stringResource(
    when (ref.kind) {
        CollectionRef.Kind.Artist -> R.string.artist
        CollectionRef.Kind.AlbumGroup -> R.string.album_group
        else -> R.string.playlist
    },
)

/** An artist's albums here, or for one only credited on tracks, their tracks. */
@Composable
internal fun Artist.subtitle(): String =
    if (albumCount > 0) pluralStringResource(R.plurals.albums, albumCount, albumCount)
    else pluralStringResource(R.plurals.tracks, trackCount, trackCount)

/** A list row with art, for recents, playlists and artists. */
@Composable
private fun LibraryRow(
    title: String,
    subtitle: String,
    art: ArtRef,
    artShape: Shape,
    onLongClick: (() -> Unit)? = null,
    onClick: () -> Unit,
) {
    ListItem(
        headlineContent = { Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = { Text(subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        leadingContent = { AlbumArt(art, contentDescription = null, shape = artShape, modifier = Modifier.size(ListArtSize)) },
        modifier = Modifier.combinedClickable(onLongClick = onLongClick, onClick = onClick),
    )
}

/** One row of the album grid; a short last row keeps the cells the same width. */
@Composable
private fun AlbumRow(albums: List<Album>, columns: Int, onOpen: (CollectionRef) -> Unit, onMenu: (Album) -> Unit) {
    Row(
        Modifier.padding(horizontal = Gutter).padding(bottom = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(CellSpacing),
    ) {
        for (album in albums) {
            Column(
                Modifier
                    .weight(1f)
                    .clip(RoundedCornerShape(12.dp))
                    .combinedClickable(onLongClick = { onMenu(album) }) {
                        onOpen(CollectionRef(CollectionRef.Kind.Album, album.uid))
                    },
            ) {
                AlbumArt(
                    album.art,
                    contentDescription = null,
                    shape = RoundedCornerShape(12.dp),
                    modifier = Modifier.fillMaxWidth().aspectRatio(1f),
                )
                Text(
                    album.title,
                    style = MaterialTheme.typography.titleSmall,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = 6.dp),
                )
                Text(
                    album.artist,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        repeat(columns - albums.size) { Spacer(Modifier.weight(1f)) }
    }
}

@Composable
private fun LazyItemScope.EmptyLibrary() {
    Column(
        Modifier.fillParentMaxWidth().fillParentMaxHeight(0.7f).padding(32.dp),
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
            stringResource(R.string.empty_title),
            style = MaterialTheme.typography.titleLarge,
            modifier = Modifier.padding(top = 16.dp),
        )
        Text(
            stringResource(R.string.empty_text_sync),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(top = 4.dp),
        )
    }
}
