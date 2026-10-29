package ng.kithso.android.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.sync.MergeState
import ng.kithso.core.CoreSync

/**
 * The merge of this phone's old library into the one it now syncs, after pairing by merging,
 * under the main screen's header as an import is: albums done of total, then playlists and
 * play counts, on one bar, while the app stays usable; then a summary of what came in, until
 * Done.
 */
@Composable
fun MergeCard(merge: MergeState, onDismiss: () -> Unit, modifier: Modifier = Modifier) {
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerHigh),
        modifier = modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(start = 16.dp, end = 16.dp, top = 16.dp, bottom = if (merge.running) 16.dp else 4.dp)) {
            val result = merge.result
            Text(
                stringResource(
                    when {
                        result == null -> R.string.merge_title
                        result.status == 0 -> R.string.merge_done_title
                        else -> R.string.merge_failed_title
                    },
                ),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
            )
            if (result == null) {
                Text(
                    stringResource(R.string.merge_text),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                LinearProgressIndicator(progress = { merge.fraction }, modifier = Modifier.fillMaxWidth().padding(top = 12.dp))
                Text(
                    stringResource(
                        if (merge.phase == CoreSync.MergePhase.Tracks) R.string.merge_progress_tracks else R.string.merge_progress_lists,
                        merge.done,
                        merge.total,
                    ),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(top = 6.dp),
                )
            } else {
                Column(Modifier.padding(top = 4.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    for (paragraph in mergeSummary(result)) {
                        Text(paragraph, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                }
                Row(Modifier.fillMaxWidth().padding(top = 4.dp), horizontalArrangement = Arrangement.End) {
                    TextButton(onClick = onDismiss) { Text(stringResource(R.string.done)) }
                }
            }
        }
    }
}

/** What the merge did, in short paragraphs, as the desktops' summary has it. */
@Composable
private fun mergeSummary(r: CoreSync.Event.MergeDone): List<String> = buildList {
    if (r.status != 0) add(stringResource(R.string.merge_failed, statusText(LocalContext.current, r.status)))
    val tracks = listOfNotNull(
        count(R.string.merge_tracks_added, r.tracksAdded),
        count(R.string.merge_tracks_better, r.tracksBetter),
        count(R.string.merge_tracks_matched, r.tracksAttached + r.tracksMapped),
        count(R.plurals.merge_tracks_deleted, r.tracksSkipped, plural = true),
        count(R.string.merge_tracks_left_out, r.tracksLeftOut),
        count(R.string.merge_tracks_failed, r.tracksFailed),
    )
    add(
        if (tracks.isEmpty()) stringResource(R.string.merge_tracks_none)
        else stringResource(R.string.merge_tracks, tracks.joinToString(stringResource(R.string.import_list_separator))),
    )
    if (r.playlists > 0) {
        val renamed = if (r.playlistsRenamed > 0) {
            " " + pluralStringResource(R.plurals.merge_playlists_renamed, r.playlistsRenamed, r.playlistsRenamed)
        } else {
            ""
        }
        add(pluralStringResource(R.plurals.merge_playlists, r.playlists, r.playlists) + renamed)
    }
    if (r.scrobbleSignedOut.isNotEmpty()) {
        val dropped = if (r.scrobblesDropped > 0) {
            " " + pluralStringResource(R.plurals.merge_scrobbles_dropped, r.scrobblesDropped, r.scrobblesDropped)
        } else {
            ""
        }
        add(stringResource(R.string.merge_scrobble_signed_out, r.scrobbleSignedOut) + dropped)
    }
    if (r.backupPath.isNotEmpty()) add(stringResource(R.string.merge_backup, r.backupPath))
}

// One item of the tracks list, or null for none: a string, or a [plural]'s.
@Composable
private fun count(id: Int, n: Int, plural: Boolean = false): String? = when {
    n <= 0 -> null
    plural -> pluralStringResource(id, n, n)
    else -> stringResource(id, n)
}
