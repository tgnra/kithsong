package app.musicapp.wear.ui

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.ui.tooling.preview.Preview
import androidx.wear.compose.foundation.lazy.TransformingLazyColumnScope
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.FilledTonalButton
import androidx.wear.compose.material3.ListHeader
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.Text

@Composable
fun HomeScreen(onAlbums: () -> Unit, onSync: () -> Unit) {
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text("musicapp")
            }
        }
        menuItem(transform, "Albums", onAlbums)
        // Not built yet.
        menuItem(transform, "Artists", null)
        menuItem(transform, "Playlists", null)
        menuItem(transform, "Sync", onSync)
        menuItem(transform, "Settings", null)
    }
}

/** A menu entry; greyed out when [onClick] is null. */
private fun TransformingLazyColumnScope.menuItem(
    transform: ItemTransform,
    label: String,
    onClick: (() -> Unit)?,
) {
    item {
        FilledTonalButton(
            onClick = { onClick?.invoke() },
            enabled = onClick != null,
            modifier = transform.modifier(this).fillMaxWidth(),
            transformation = transform.surface(this),
            label = { Text(label) },
        )
    }
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun HomeScreenPreview() {
    MaterialTheme {
        AppScaffold {
            HomeScreen(onAlbums = {}, onSync = {})
        }
    }
}
