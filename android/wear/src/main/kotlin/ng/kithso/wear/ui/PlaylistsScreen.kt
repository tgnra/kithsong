package ng.kithso.wear.ui

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.wear.compose.foundation.lazy.items
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.Button
import androidx.wear.compose.material3.ButtonDefaults
import androidx.wear.compose.material3.FilledTonalButton
import androidx.wear.compose.material3.ListHeader
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.Text
import ng.kithso.core.PlaylistInfo
import ng.kithso.core.TrackInfo
import ng.kithso.wear.R

/** The playlists with a track on the watch, each with how many of its tracks are here. */
@Composable
fun PlaylistsScreen(playlists: List<PlaylistInfo>, onPlaylist: (PlaylistInfo) -> Unit) {
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(stringResource(R.string.section_playlists))
            }
        }
        if (playlists.isEmpty()) {
            item {
                Text(
                    stringResource(R.string.watch_no_playlists),
                    modifier = transform.modifier(this).fillMaxWidth().padding(horizontal = 8.dp),
                    style = MaterialTheme.typography.bodyMedium,
                    textAlign = TextAlign.Center,
                )
            }
        }
        items(playlists, key = { it.uid }) { playlist ->
            FilledTonalButton(
                onClick = { onPlaylist(playlist) },
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
                label = { Text(playlist.name, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                secondaryLabel = {
                    Text(pluralStringResource(R.plurals.tracks, playlist.playableCount, playlist.playableCount))
                },
            )
        }
    }
}

/**
 * [playlist]'s [tracks] that are on the watch, in playlist order, each with its artist since
 * they come from different albums. [playingUid] is the track the player is on, if it is one
 * of them; a long press opens the track's menu ([onTrackMenu]).
 */
@Composable
fun PlaylistScreen(
    playlist: PlaylistInfo,
    tracks: List<TrackInfo>,
    playingUid: String?,
    onTrack: (TrackInfo) -> Unit,
    onTrackMenu: (TrackInfo) -> Unit,
) {
    val haptics = LocalHapticFeedback.current
    val menuLabel = stringResource(R.string.track_menu)
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(playlist.name, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
        }
        // A track can be in a playlist twice, so the key is its place as well.
        items(tracks.size, key = { "$it/${tracks[it].uid}" }) { index ->
            val track = tracks[index]
            Button(
                onClick = { onTrack(track) },
                onLongClick = {
                    haptics.longPressed()
                    onTrackMenu(track)
                },
                onLongClickLabel = menuLabel,
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
                colors = if (track.uid == playingUid) {
                    ButtonDefaults.buttonColors()
                } else {
                    ButtonDefaults.filledTonalButtonColors()
                },
                label = { Text(track.title, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                secondaryLabel = {
                    Text(track.artist.orEmpty(), maxLines = 1, overflow = TextOverflow.Ellipsis)
                },
            )
        }
    }
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun PlaylistsScreenPreview() {
    MaterialTheme {
        AppScaffold {
            PlaylistsScreen(PreviewData.playlists, onPlaylist = {})
        }
    }
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun PlaylistScreenPreview() {
    MaterialTheme {
        AppScaffold {
            PlaylistScreen(
                PreviewData.playlists.first(),
                PreviewData.tracks,
                playingUid = PreviewData.tracks[2].uid,
                onTrack = {},
                onTrackMenu = {},
            )
        }
    }
}
