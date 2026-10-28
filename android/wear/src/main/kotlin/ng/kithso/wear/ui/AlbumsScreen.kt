package ng.kithso.wear.ui

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
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

@Composable
fun AlbumsScreen(albums: List<AlbumInfo>, onAlbum: (AlbumInfo) -> Unit) {
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(stringResource(R.string.section_albums))
            }
        }
        if (albums.isEmpty()) {
            item {
                Text(
                    stringResource(R.string.watch_empty_text),
                    modifier = transform.modifier(this).fillMaxWidth().padding(horizontal = 8.dp),
                    style = MaterialTheme.typography.bodyMedium,
                    textAlign = TextAlign.Center,
                )
            }
        }
        items(albums, key = { it.uid }) { album ->
            FilledTonalButton(
                onClick = { onAlbum(album) },
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
            AlbumsScreen(PreviewData.albums, onAlbum = {})
        }
    }
}
