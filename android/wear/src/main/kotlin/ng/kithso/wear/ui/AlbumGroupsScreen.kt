package ng.kithso.wear.ui

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.pluralStringResource
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
import ng.kithso.core.NamedInfo
import ng.kithso.wear.R

/** The album groups with an album on the watch, each with how many of its albums are here. */
@Composable
fun AlbumGroupsScreen(groups: List<NamedInfo>, onGroup: (NamedInfo) -> Unit) {
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(stringResource(R.string.section_album_groups))
            }
        }
        if (groups.isEmpty()) {
            item {
                Text(
                    stringResource(R.string.watch_no_album_groups),
                    modifier = transform.modifier(this).fillMaxWidth().padding(horizontal = 8.dp),
                    style = MaterialTheme.typography.bodyMedium,
                    textAlign = TextAlign.Center,
                )
            }
        }
        items(groups, key = { it.uid }) { group ->
            FilledTonalButton(
                onClick = { onGroup(group) },
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
                label = { Text(group.name, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                secondaryLabel = {
                    Text(pluralStringResource(R.plurals.count_albums, group.albumCount, group.albumCount))
                },
            )
        }
    }
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun AlbumGroupsScreenPreview() {
    MaterialTheme {
        AppScaffold {
            AlbumGroupsScreen(PreviewData.albumGroups, onGroup = {})
        }
    }
}
