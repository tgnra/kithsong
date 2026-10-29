package ng.kithso.wear.ui

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.runtime.Composable
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.tooling.preview.Preview
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.FilledTonalButton
import androidx.wear.compose.material3.ListHeader
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.Text
import ng.kithso.wear.R

/** The watch's settings. So far only the licences. */
@Composable
fun SettingsScreen(onLicenses: () -> Unit) {
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(stringResource(R.string.settings))
            }
        }
        item {
            FilledTonalButton(
                onClick = onLicenses,
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
                label = { Text(stringResource(R.string.watch_licenses)) },
            )
        }
    }
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun SettingsScreenPreview() {
    MaterialTheme {
        AppScaffold {
            SettingsScreen(onLicenses = {})
        }
    }
}
