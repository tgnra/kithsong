package ng.kithso.wear.ui

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
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

/** One line of a [ChoiceScreen]; [onChoose] is what choosing it does. */
class Choice(val key: String, val label: String, val onChoose: () -> Unit)

/**
 * A title over a short list of [choices]: a track's or album's menu, or the playlists or
 * album groups to add to. Swiping right leaves without choosing, as on every screen, so
 * there's no Cancel. [emptyText] shows when there's nothing to choose.
 */
@Composable
fun ChoiceScreen(title: String, choices: List<Choice>, emptyText: String? = null) {
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(title, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
        }
        if (choices.isEmpty() && emptyText != null) {
            item {
                Text(
                    emptyText,
                    modifier = transform.modifier(this).fillMaxWidth().padding(horizontal = 8.dp),
                    style = MaterialTheme.typography.bodyMedium,
                    textAlign = TextAlign.Center,
                )
            }
        }
        items(choices, key = { it.key }) { choice ->
            FilledTonalButton(
                onClick = choice.onChoose,
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
                label = { Text(choice.label, maxLines = 2, overflow = TextOverflow.Ellipsis) },
            )
        }
    }
}

@Preview(device = "id:wearos_small_round", showSystemUi = true)
@Composable
private fun ChoiceScreenPreview() {
    MaterialTheme {
        AppScaffold {
            ChoiceScreen(
                "Add to playlist",
                PreviewData.playlists.map { Choice(it.uid, it.name) {} },
            )
        }
    }
}
