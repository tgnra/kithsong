package ng.kithso.wear.ui

import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.wear.compose.foundation.lazy.items
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.Button
import androidx.wear.compose.material3.ButtonDefaults
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.Text
import ng.kithso.core.AlbumInfo
import ng.kithso.core.TrackInfo
import ng.kithso.wear.R

/**
 * [album]'s cover and [tracks]. [playingUid] is the track the player is on, if it is one of
 * them. A long press on the cover opens the album's menu ([onAlbumMenu]), one on a track the
 * track's ([onTrackMenu]).
 */
@Composable
fun AlbumScreen(
    album: AlbumInfo,
    tracks: List<TrackInfo>,
    playingUid: String?,
    onTrack: (TrackInfo) -> Unit,
    onAlbumMenu: () -> Unit,
    onTrackMenu: (TrackInfo) -> Unit,
) {
    val haptics = LocalHapticFeedback.current
    val albumMenuLabel = stringResource(R.string.album_menu)
    val trackMenuLabel = stringResource(R.string.track_menu)
    ListScreen { transform ->
        item {
            Column(
                // Not transformed: shrinking would distort the cover. The header isn't a
                // button, so the long press is its own gesture, with the menu as an action
                // for accessibility.
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 12.dp, bottom = 8.dp)
                    .pointerInput(onAlbumMenu) {
                        detectTapGestures(
                            onLongPress = {
                                haptics.longPressed()
                                onAlbumMenu()
                            },
                        )
                    }
                    .semantics {
                        customActions = listOf(
                            CustomAccessibilityAction(albumMenuLabel) {
                                onAlbumMenu()
                                true
                            },
                        )
                    },
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                AlbumArt(album, 56.dp)
                Spacer(Modifier.height(6.dp))
                Text(
                    album.title,
                    style = MaterialTheme.typography.titleSmall,
                    textAlign = TextAlign.Center,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    album.artist.orEmpty(),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        items(tracks, key = { it.uid }) { track ->
            Button(
                onClick = { onTrack(track) },
                onLongClick = {
                    haptics.longPressed()
                    onTrackMenu(track)
                },
                onLongClickLabel = trackMenuLabel,
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
                // The playing track stands out in the primary colour.
                colors = if (track.uid == playingUid) {
                    ButtonDefaults.buttonColors()
                } else {
                    ButtonDefaults.filledTonalButtonColors()
                },
                icon = {
                    Text(
                        if (track.trackNo > 0) track.trackNo.toString() else "",
                        modifier = Modifier.widthIn(min = 20.dp),
                        textAlign = TextAlign.Center,
                    )
                },
                label = { Text(track.title, maxLines = 2, overflow = TextOverflow.Ellipsis) },
            )
        }
    }
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun AlbumScreenPreview() {
    MaterialTheme {
        AppScaffold {
            AlbumScreen(
                PreviewData.albums.first(),
                PreviewData.tracks,
                playingUid = PreviewData.tracks[1].uid,
                onTrack = {},
                onAlbumMenu = {},
                onTrackMenu = {},
            )
        }
    }
}
