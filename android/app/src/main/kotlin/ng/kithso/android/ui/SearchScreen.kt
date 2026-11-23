package ng.kithso.android.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.width
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
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalResources
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.FoundTrack
import ng.kithso.android.data.LibrarySource
import ng.kithso.android.data.SearchResults
import ng.kithso.android.ui.art.AlbumArt
import kotlinx.coroutines.delay

/**
 * Search: results by kind, the kind with the best match first, as on the desktops. The core's
 * search is quick, so results follow the typing after a short pause.
 */
@Composable
fun SearchScreen(
    source: LibrarySource,
    bottomPadding: Dp,
    onOpen: (CollectionRef) -> Unit,
    onPlayTrack: (FoundTrack) -> Unit,
    onTrackMenu: (FoundTrack) -> Unit,
    onBack: () -> Unit,
) {
    var query by rememberSaveable { mutableStateOf("") }
    val results = rememberSearchResults(source, query)
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    val focusManager = LocalFocusManager.current

    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .windowInsetsPadding(pageTop())
                    .windowInsetsPadding(pageSides())
                    .padding(start = 4.dp, end = 16.dp, top = 8.dp, bottom = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                // Not for the tablet layout's Search, which its sidebar picked.
                if (LocalPageChrome.current.back != PageBack.None) {
                    IconButton(onClick = onBack) { Icon(Icons.ArrowBack, contentDescription = stringResource(R.string.back)) }
                } else {
                    Spacer(Modifier.width(12.dp))
                }
                TextField(
                    value = query,
                    onValueChange = { query = it },
                    placeholder = { Text(stringResource(R.string.search_placeholder), maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    leadingIcon = { Icon(Icons.Search, contentDescription = null) },
                    trailingIcon = if (query.isNotEmpty()) {
                        {
                            IconButton(onClick = { query = "" }) {
                                Icon(Icons.Clear, contentDescription = stringResource(R.string.clear))
                            }
                        }
                    } else null,
                    singleLine = true,
                    shape = RoundedCornerShape(28.dp),
                    colors = TextFieldDefaults.colors(
                        focusedIndicatorColor = Color.Transparent,
                        unfocusedIndicatorColor = Color.Transparent,
                    ),
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                    keyboardActions = KeyboardActions(onSearch = { focusManager.clearFocus() }),
                    modifier = Modifier.weight(1f).padding(start = 4.dp).focusRequester(focus),
                )
            }
            LazyColumn(
                contentPadding = pageSides()
                    .add(WindowInsets(bottom = bottomPadding))
                    .asPaddingValues(),
                modifier = Modifier.fillMaxSize(),
            ) {
                searchResults(query, results, onOpen, onPlayTrack, onTrackMenu)
            }
        }
    }
}

/**
 * What the core found for [query], following the typing after a short pause. Null until the
 * first search is back, and for a blank query. A new query keeps the last one's results until
 * its own are back, so the list doesn't blink with each letter.
 */
@Composable
fun rememberSearchResults(source: LibrarySource, query: String): SearchResults? {
    var results by remember { mutableStateOf<SearchResults?>(null) }
    LaunchedEffect(query) {
        if (query.isBlank()) {
            results = null
            return@LaunchedEffect
        }
        delay(SEARCH_DELAY_MS)
        results = source.search(query)
    }
    return results.takeIf { query.isNotBlank() }
}

/**
 * [found]'s results by kind, the kind with the best match first, then how many of each there
 * are ("8 tracks, 1 album", "20+ tracks" for a kind with more than are listed), so the end of
 * the list shows the keyboard isn't hiding any; or that nothing matched [query].
 */
fun LazyListScope.searchResults(
    query: String,
    found: SearchResults?,
    onOpen: (CollectionRef) -> Unit,
    onPlayTrack: (FoundTrack) -> Unit,
    onTrackMenu: (FoundTrack) -> Unit,
) {
    when {
        found == null || query.isBlank() -> Unit
        found.isEmpty -> item(key = "none") { NoResults(query) }
        else -> {
            for (kind in found.order) results(kind, found, onOpen, onPlayTrack, onTrackMenu)
            item(key = "totals", contentType = "totals") { Totals(found) }
        }
    }
}

@Composable
private fun Totals(found: SearchResults) {
    val res = LocalResources.current
    val text = found.order.joinToString(stringResource(R.string.import_list_separator)) { kind ->
        val more = kind in found.more
        val (plural, count) = when (kind) {
            SearchResults.Kind.Artists -> (if (more) R.plurals.search_more_artists else R.plurals.count_artists) to found.artists.size
            SearchResults.Kind.Albums -> (if (more) R.plurals.search_more_albums else R.plurals.count_albums) to found.albums.size
            SearchResults.Kind.Tracks -> (if (more) R.plurals.search_more_tracks else R.plurals.tracks) to found.tracks.size
            SearchResults.Kind.Playlists ->
                (if (more) R.plurals.search_more_playlists else R.plurals.count_playlists) to found.playlists.size
            SearchResults.Kind.AlbumGroups ->
                (if (more) R.plurals.search_more_album_groups else R.plurals.count_album_groups) to found.albumGroups.size
        }
        res.getQuantityString(plural, count, count)
    }
    Text(
        text,
        style = MaterialTheme.typography.bodyMedium,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        textAlign = TextAlign.Center,
        modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 24.dp),
    )
}

private const val SEARCH_DELAY_MS = 150L

// One kind's results, under its heading.
private fun LazyListScope.results(
    kind: SearchResults.Kind,
    found: SearchResults,
    onOpen: (CollectionRef) -> Unit,
    onPlayTrack: (FoundTrack) -> Unit,
    onTrackMenu: (FoundTrack) -> Unit,
) {
    val heading = when (kind) {
        SearchResults.Kind.Artists -> R.string.section_artists
        SearchResults.Kind.Albums -> R.string.section_albums
        SearchResults.Kind.Tracks -> R.string.tracks_title
        SearchResults.Kind.Playlists -> R.string.section_playlists
        SearchResults.Kind.AlbumGroups -> R.string.section_album_groups
    }
    item(key = "heading:$kind", contentType = "heading") { BrowseHeading(stringResource(heading)) }
    val rounded = RoundedCornerShape(8.dp)
    when (kind) {
        SearchResults.Kind.Artists -> items(found.artists, key = { "artist:${it.uid}" }) { artist ->
            ResultRow(artist.name, artist.subtitle(), artist.art, CircleShape) {
                onOpen(CollectionRef(CollectionRef.Kind.Artist, artist.uid))
            }
        }
        SearchResults.Kind.Albums -> items(found.albums, key = { "album:${it.uid}" }) { album ->
            ResultRow(album.title, album.artist, album.art, rounded, drag = DragItem.Album(album.uid, album.title, album.artist)) {
                onOpen(CollectionRef(CollectionRef.Kind.Album, album.uid))
            }
        }
        SearchResults.Kind.Tracks -> items(found.tracks, key = { "track:${it.uid}" }) { track ->
            ResultRow(
                track.title,
                trackByline(track.artist, track.album),
                track.art,
                rounded,
                onLongClick = { onTrackMenu(track) },
                drag = DragItem.Track(track.uid, track.title, track.artist),
            ) { onPlayTrack(track) }
        }
        SearchResults.Kind.Playlists -> items(found.playlists, key = { "playlist:${it.uid}" }) { playlist ->
            ResultRow(playlist.name, stringResource(R.string.playlist), playlist.art, rounded) {
                onOpen(CollectionRef(CollectionRef.Kind.Playlist, playlist.uid))
            }
        }
        SearchResults.Kind.AlbumGroups -> items(found.albumGroups, key = { "group:${it.uid}" }) { group ->
            ResultRow(group.name, stringResource(R.string.album_group), group.art, rounded) {
                onOpen(CollectionRef(CollectionRef.Kind.AlbumGroup, group.uid))
            }
        }
    }
}

/** A result, which in the tablet layout drags [drag], if given, as a track list's tracks and the albums' tiles do. */
@Composable
private fun ResultRow(
    title: String,
    subtitle: String,
    art: ArtRef,
    artShape: Shape,
    onLongClick: (() -> Unit)? = null,
    drag: DragItem? = null,
    onClick: () -> Unit,
) {
    ListItem(
        headlineContent = { Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = { Text(subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        leadingContent = { AlbumArt(art, contentDescription = null, shape = artShape, modifier = Modifier.size(48.dp)) },
        // Over what's behind: the Search page's surface, or the phone's art backdrop under its header.
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.listItemKeys(ListRowFocusShape, onLongClick).menuClickable(onLongClick, onClick, drag),
    )
}

@Composable
private fun NoResults(query: String) {
    Column(
        Modifier.fillMaxWidth().padding(horizontal = 32.dp, vertical = 48.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(stringResource(R.string.search_no_results_title), style = MaterialTheme.typography.titleLarge)
        Text(
            stringResource(R.string.search_no_results_text, query.trim()),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(top = 4.dp),
        )
    }
}
