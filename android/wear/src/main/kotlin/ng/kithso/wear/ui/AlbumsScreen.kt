package ng.kithso.wear.ui

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.wear.compose.foundation.lazy.items
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.FilledTonalButton
import androidx.wear.compose.material3.ListHeader
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.Text
import ng.kithso.core.AlbumInfo
import ng.kithso.wear.R

/**
 * [albums] under [title]: every album on the watch, or an album group's that are here.
 * [emptyText] shows when there are none. A long press on an album opens its menu
 * ([onAlbumMenu]).
 */
@Composable
fun AlbumsScreen(
    albums: List<AlbumInfo>,
    title: String = stringResource(R.string.section_albums),
    emptyText: String? = stringResource(R.string.watch_empty_text),
    onAlbum: (AlbumInfo) -> Unit,
    onAlbumMenu: (AlbumInfo) -> Unit,
) {
    val haptics = LocalHapticFeedback.current
    val menuLabel = stringResource(R.string.album_menu)
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(title, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
        }
        if (albums.isEmpty() && emptyText != null) {
            item {
                Text(
                    emptyText,
                    modifier = transform.modifier(this).fillMaxWidth().padding(horizontal = 8.dp),
                    style = MaterialTheme.typography.bodyMedium,
                    textAlign = TextAlign.Center,
                )
            }
        }
        items(albums, key = { it.uid }) { album ->
            FilledTonalButton(
                onClick = { onAlbum(album) },
                onLongClick = {
                    haptics.longPressed()
                    onAlbumMenu(album)
                },
                onLongClickLabel = menuLabel,
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
                icon = { AlbumArt(album, ListArtSize) },
                label = { Text(album.title, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                secondaryLabel = {
                    Text(album.artist.orEmpty(), maxLines = 1, overflow = TextOverflow.Ellipsis)
                },
            )
        }
    }
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun AlbumsScreenPreview() {
    MaterialTheme {
        AppScaffold {
            AlbumsScreen(PreviewData.albums, onAlbum = {}, onAlbumMenu = {})
        }
    }
}
