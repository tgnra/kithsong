package ng.kithso.android.ui

import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.android.data.Album
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.ui.art.AlbumArt

// Album tiles for the screens over the main one: an artist's albums in a strip, an album
// group's in a grid. Laid out as the main screen's album grid is.

internal val BrowseGutter = 16.dp
private val TileSpacing = 12.dp
private val MinTileWidth = 150.dp
private val StripTileWidth = 140.dp

/** How many album tiles fit across [width], as on the main screen. */
internal fun albumColumns(width: Dp): Int =
    ((width - BrowseGutter * 2 + TileSpacing) / (MinTileWidth + TileSpacing)).toInt().coerceAtLeast(2)

/** A heading over part of a screen, such as an artist's albums. */
@Composable
internal fun BrowseHeading(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.titleLarge,
        modifier = Modifier
            .padding(start = BrowseGutter, end = BrowseGutter, top = 16.dp, bottom = 8.dp)
            .semantics { heading() },
    )
}

/** An album's cover, title and [subtitle]; tapping it opens the album, long-pressing it its menu. */
@Composable
private fun AlbumTile(
    album: Album,
    subtitle: String,
    onOpen: (CollectionRef) -> Unit,
    onMenu: (Album) -> Unit,
    modifier: Modifier,
) {
    Column(
        modifier
            .clip(RoundedCornerShape(12.dp))
            .combinedClickable(onLongClick = { onMenu(album) }) { onOpen(CollectionRef(CollectionRef.Kind.Album, album.uid)) },
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
            subtitle,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
    }
}

/** One row of an album grid; a short last row keeps the tiles the same width. */
@Composable
internal fun AlbumGridRow(albums: List<Album>, columns: Int, onOpen: (CollectionRef) -> Unit, onMenu: (Album) -> Unit) {
    Row(
        Modifier.padding(horizontal = BrowseGutter).padding(bottom = 16.dp),
        horizontalArrangement = Arrangement.spacedBy(TileSpacing),
    ) {
        for (album in albums) AlbumTile(album, album.artist, onOpen, onMenu, Modifier.weight(1f))
        repeat(columns - albums.size) { Spacer(Modifier.weight(1f)) }
    }
}

/**
 * An artist's albums, scrolling sideways. An album by someone else (one the artist is only
 * credited on) shows whose it is; the artist's own show their year.
 */
@Composable
internal fun AlbumStrip(albums: List<Album>, artist: String, onOpen: (CollectionRef) -> Unit, onMenu: (Album) -> Unit) {
    LazyRow(
        contentPadding = PaddingValues(horizontal = BrowseGutter),
        horizontalArrangement = Arrangement.spacedBy(TileSpacing),
    ) {
        items(albums, key = { it.uid }) { album ->
            val subtitle = if (album.artist != artist) album.artist else if (album.year > 0) album.year.toString() else ""
            AlbumTile(album, subtitle, onOpen, onMenu, Modifier.width(StripTileWidth))
        }
    }
}
