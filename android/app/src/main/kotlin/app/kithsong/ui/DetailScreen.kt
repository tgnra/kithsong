package app.kithsong.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
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
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import app.kithsong.R
import app.kithsong.data.CollectionRef
import app.kithsong.data.LibrarySource
import app.kithsong.data.Track
import app.kithsong.data.TrackList
import app.kithsong.ui.art.AlbumArt

/**
 * An album, artist or playlist: its art and name, then its tracks. Tapping a track plays the
 * list from there. [onBack] slides the screen away; it is also dismissed by swiping right.
 */
@Composable
fun DetailScreen(
    ref: CollectionRef,
    source: LibrarySource,
    bottomPadding: Dp,
    onPlay: (List<Track>, Int) -> Unit,
    onBack: () -> Unit,
) {
    val loaded by produceState<TrackList?>(null, ref) { value = source.trackList(ref) }
    // A playlist's picture is looked up after the library is read, so it may come later.
    val library by source.library.collectAsState()
    val playlistArt = library?.playlists?.firstOrNull { it.uid == ref.uid }?.art
        ?.takeIf { ref.kind == CollectionRef.Kind.Playlist }
    val list = loaded?.let { if (playlistArt != null) it.copy(art = playlistArt) else it }
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        val tracks = list?.tracks.orEmpty()
        LazyColumn(
            contentPadding = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
                .add(WindowInsets(bottom = bottomPadding))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "header", contentType = "header") {
                DetailHeader(ref.kind, list, onBack)
            }
            // A playlist can hold a track twice, so the uid alone isn't a unique key.
            itemsIndexed(tracks, key = { i, track -> "$i:${track.uid}" }, contentType = { _, _ -> "track" }) { i, track ->
                TrackRow(track, showAlbum = ref.kind != CollectionRef.Kind.Album) { onPlay(tracks, i) }
            }
        }
    }
}

/**
 * The back button, then the art, title and subtitle beside each other. In a short window
 * (landscape, mostly) the back button goes beside them too, and the art is smaller, to leave
 * room for the tracks.
 */
@Composable
private fun DetailHeader(kind: CollectionRef.Kind, list: TrackList?, onBack: () -> Unit) {
    val short = isShortWindow()
    val back = @Composable {
        IconButton(onClick = onBack, modifier = Modifier.padding(start = 4.dp, top = 4.dp)) {
            Icon(Icons.ArrowBack, contentDescription = stringResource(R.string.back))
        }
    }
    if (short) {
        Row(Modifier.fillMaxWidth().windowInsetsPadding(WindowInsets.statusBars)) {
            back()
            if (list != null) DetailTitle(kind, list, 96.dp, Modifier.padding(start = 4.dp, top = 12.dp))
        }
    } else {
        Column(Modifier.fillMaxWidth().windowInsetsPadding(WindowInsets.statusBars)) {
            back()
            if (list != null) DetailTitle(kind, list, 128.dp, Modifier.padding(start = 16.dp))
        }
    }
}

@Composable
private fun DetailTitle(kind: CollectionRef.Kind, list: TrackList, artSize: Dp, modifier: Modifier) {
    Row(modifier.padding(end = 16.dp, bottom = 16.dp), verticalAlignment = Alignment.CenterVertically) {
        AlbumArt(
            list.art,
            contentDescription = when (kind) {
                CollectionRef.Kind.Album -> stringResource(R.string.cover_of, list.title)
                else -> null
            },
            shape = if (kind == CollectionRef.Kind.Artist) CircleShape else RoundedCornerShape(16.dp),
            modifier = Modifier.size(artSize),
        )
        Column(Modifier.padding(start = 16.dp).widthIn(min = 0.dp)) {
            Text(
                list.title,
                style = MaterialTheme.typography.headlineSmall,
                maxLines = 3,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.semantics { heading() },
            )
            Text(
                when (kind) {
                    CollectionRef.Kind.Album ->
                        if (list.year > 0) stringResource(R.string.album_byline_year, list.artist, list.year) else list.artist
                    CollectionRef.Kind.Artist -> pluralStringResource(R.plurals.albums, list.albumCount, list.albumCount)
                    CollectionRef.Kind.Playlist -> pluralStringResource(R.plurals.tracks, list.tracks.size, list.tracks.size)
                },
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun TrackRow(track: Track, showAlbum: Boolean, onClick: () -> Unit) {
    ListItem(
        leadingContent = if (showAlbum) {
            { AlbumArt(track.art, contentDescription = null, shape = RoundedCornerShape(6.dp), modifier = Modifier.size(40.dp)) }
        } else {
            {
                Box(Modifier.size(40.dp), contentAlignment = Alignment.Center) {
                    Text(
                        track.trackNo.toString(),
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        },
        headlineContent = { Text(track.title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = if (showAlbum) {
            { Text(stringResource(R.string.track_byline, track.artist, track.album), maxLines = 1, overflow = TextOverflow.Ellipsis) }
        } else null,
        trailingContent = { Text(formatTime(track.durationMs)) },
        modifier = Modifier.clickable(onClick = onClick),
    )
}
