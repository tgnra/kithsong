package ng.kithso.wear.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.wear.compose.foundation.lazy.TransformingLazyColumnScope
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.FilledTonalButton
import androidx.wear.compose.material3.ListHeader
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.Text
import ng.kithso.core.TrackInfo
import ng.kithso.wear.R

/** The menu. [nowPlaying] is the player's track, whose entry leads back to it; null when nothing is queued. */
@Composable
fun HomeScreen(
    nowPlaying: TrackInfo?,
    onNowPlaying: () -> Unit,
    onAlbums: () -> Unit,
    onSync: () -> Unit,
    onSettings: () -> Unit,
) {
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(stringResource(R.string.app_name))
            }
        }
        if (nowPlaying != null) {
            item {
                FilledTonalButton(
                    onClick = onNowPlaying,
                    modifier = transform.modifier(this).fillMaxWidth(),
                    transformation = transform.surface(this),
                    label = { Text(stringResource(R.string.watch_now_playing)) },
                    secondaryLabel = {
                        Text(
                            nowPlaying.artist?.let { stringResource(R.string.watch_title_artist, nowPlaying.title, it) }
                                ?: nowPlaying.title,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                        )
                    },
                )
            }
        }
        menuItem(transform, R.string.section_albums, onAlbums)
        // Not built yet.
        menuItem(transform, R.string.section_artists, null)
        menuItem(transform, R.string.section_playlists, null)
        menuItem(transform, R.string.sync, onSync)
        menuItem(transform, R.string.settings, onSettings)
    }
}

/** A menu entry; greyed out when [onClick] is null. */
private fun TransformingLazyColumnScope.menuItem(
    transform: ItemTransform,
    @StringRes label: Int,
    onClick: (() -> Unit)?,
) {
    item {
        FilledTonalButton(
            onClick = { onClick?.invoke() },
            enabled = onClick != null,
            modifier = transform.modifier(this).fillMaxWidth(),
            transformation = transform.surface(this),
            label = { Text(stringResource(label)) },
        )
    }
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun HomeScreenPreview() {
    MaterialTheme {
        AppScaffold {
            HomeScreen(PreviewData.tracks[1], onNowPlaying = {}, onAlbums = {}, onSync = {}, onSettings = {})
        }
    }
}
