package ng.kithso.wear.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.wear.compose.foundation.lazy.TransformingLazyColumnScope
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.FilledTonalButton
import androidx.wear.compose.material3.ListHeader
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.Text
import ng.kithso.wear.R

@Composable
fun HomeScreen(onAlbums: () -> Unit, onSync: () -> Unit, onSettings: () -> Unit) {
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(stringResource(R.string.app_name))
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
            HomeScreen(onAlbums = {}, onSync = {}, onSettings = {})
        }
    }
}
