package ng.kithso.android.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
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
 * Search: a box at the top, and what matches beneath, by kind, the kind with the best match
 * first, as on the desktops. A result opens its page; a track plays its album from there
 * ([onPlayTrack]). The core's search is quick, so results follow the typing after a short
 * pause. [onBack] slides the screen away; it is also dismissed by swiping right.
 */
@Composable
fun SearchScreen(
    source: LibrarySource,
    bottomPadding: Dp,
    onOpen: (CollectionRef) -> Unit,
    onPlayTrack: (FoundTrack) -> Unit,
    onBack: () -> Unit,
) {
    var query by rememberSaveable { mutableStateOf("") }
    // Null until the first search is back. A new query keeps the last one's results until
    // its own are back, so the list doesn't blink with each letter.
    var results by remember { mutableStateOf<SearchResults?>(null) }
    LaunchedEffect(query) {
        if (query.isBlank()) {
            results = null
            return@LaunchedEffect
        }
        delay(SEARCH_DELAY_MS)
        results = source.search(query)
    }
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    val focusManager = LocalFocusManager.current

    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            Row(
                Modifier
                    .fillMaxWidth()
                    .windowInsetsPadding(WindowInsets.statusBars)
                    .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal))
                    .padding(start = 4.dp, end = 16.dp, top = 8.dp, bottom = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                IconButton(onClick = onBack) { Icon(Icons.ArrowBack, contentDescription = stringResource(R.string.back)) }
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
            val found = results
            LazyColumn(
                contentPadding = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
                    .add(WindowInsets(bottom = bottomPadding))
                    .asPaddingValues(),
                modifier = Modifier.fillMaxSize(),
            ) {
                when {
                    found == null || query.isBlank() -> Unit
                    found.isEmpty -> item(key = "none") { NoResults(query) }
                    else -> for (kind in found.order) results(kind, found, onOpen, onPlayTrack)
                }
            }
        }
    }
}

private const val SEARCH_DELAY_MS = 150L

// One kind's results, under its heading.
private fun LazyListScope.results(
    kind: SearchResults.Kind,
    found: SearchResults,
    onOpen: (CollectionRef) -> Unit,
    onPlayTrack: (FoundTrack) -> Unit,
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
            ResultRow(album.title, album.artist, album.art, rounded) {
                onOpen(CollectionRef(CollectionRef.Kind.Album, album.uid))
            }
        }
        SearchResults.Kind.Tracks -> items(found.tracks, key = { "track:${it.uid}" }) { track ->
            ResultRow(track.title, trackByline(track.artist, track.album), track.art, rounded) {
                onPlayTrack(track)
            }
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

@Composable
private fun ResultRow(title: String, subtitle: String, art: ArtRef, artShape: Shape, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = { Text(subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        leadingContent = { AlbumArt(art, contentDescription = null, shape = artShape, modifier = Modifier.size(48.dp)) },
        modifier = Modifier.clickable(onClick = onClick),
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
