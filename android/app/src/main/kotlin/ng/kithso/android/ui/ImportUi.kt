package ng.kithso.android.ui

import android.content.ActivityNotFoundException
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.importer.ImportProgress
import ng.kithso.android.importer.ImportSource
import ng.kithso.android.importer.Importer
import ng.kithso.android.importer.detail
import ng.kithso.android.importer.title

/** Opens the system's pickers for an import; see [rememberImportPickers]. */
@Stable
class ImportPickers(val pickFolder: () -> Unit, val pickFiles: () -> Unit)

/**
 * The Storage Access Framework's pickers, which need no storage permission: a folder
 * (OpenDocumentTree) or audio files and zips (OpenMultipleDocuments). What's picked starts an
 * import in [importer], and [onStarted] is called; if one's running it joins the end of that.
 */
@Composable
fun rememberImportPickers(importer: Importer, onStarted: () -> Unit): ImportPickers {
    val folder = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { tree ->
        if (tree != null) begin(importer, ImportSource.Folder(tree), onStarted)
    }
    val files = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) begin(importer, ImportSource.Documents(uris), onStarted)
    }
    return remember(folder, files) {
        ImportPickers(
            pickFolder = { launch { folder.launch(null) } },
            pickFiles = { launch { files.launch(PICKED_TYPES) } },
        )
    }
}

private fun begin(importer: Importer, what: ImportSource, onStarted: () -> Unit) {
    if (importer.start(what)) onStarted() else importer.add(what)
}

// Audio files, and zips such as Bandcamp's downloads.
private val PICKED_TYPES = arrayOf("audio/*", "application/zip", "application/x-zip-compressed")

private inline fun launch(open: () -> Unit) {
    try {
        open()
    } catch (e: ActivityNotFoundException) {
        // No documents UI, as on some stripped-down devices.
        Log.w("kithsong", "No picker", e)
    }
}

/**
 * A menu's ways to import: a folder, or files and zips, which join an import that's running.
 * [onDismiss] closes the menu.
 */
@Composable
fun ImportChoices(pickers: ImportPickers, onDismiss: () -> Unit) {
    DropdownMenuItem(
        text = { Text(stringResource(R.string.import_folder)) },
        onClick = {
            onDismiss()
            pickers.pickFolder()
        },
    )
    DropdownMenuItem(
        text = { Text(stringResource(R.string.import_files)) },
        onClick = {
            onDismiss()
            pickers.pickFiles()
        },
    )
}

/** The empty library's Import button's menu: [ImportChoices] alone. */
@Composable
fun ImportMenu(expanded: Boolean, pickers: ImportPickers, onDismiss: () -> Unit) {
    DropdownMenu(expanded = expanded, onDismissRequest = onDismiss) {
        ImportChoices(pickers, onDismiss)
    }
}

/**
 * How the import is going, under the main screen's header: what it's doing, a progress bar,
 * and a way to cancel; the offline banner, with ways to retry MusicBrainz and to import without
 * tagging (also shown while items couldn't be looked up though it's reachable); the items
 * with no sure match, to settle all at once or to review one by one, which [onReview] opens
 * (the import's page, [ImportScreen]); then a summary with what failed and what the library
 * already had, until dismissed.
 */
@Composable
fun ImportCard(progress: ImportProgress, importer: Importer, onReview: () -> Unit, modifier: Modifier = Modifier) {
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerHigh),
        modifier = modifier.fillMaxWidth(),
    ) {
        val context = LocalContext.current
        Column(Modifier.padding(start = 16.dp, end = 8.dp, top = 16.dp)) {
            Text(
                progress.title(context),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(end = 8.dp).semantics { liveRegion = LiveRegionMode.Polite },
            )
            Text(
                progress.detail(context),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(end = 8.dp),
            )
            val bar = Modifier.fillMaxWidth().padding(top = 12.dp, end = 8.dp)
            when (progress.phase) {
                ImportProgress.Phase.Copying ->
                    if (progress.toCopy > 0) LinearProgressIndicator(progress = { progress.copied.toFloat() / progress.toCopy }, modifier = bar)
                    else LinearProgressIndicator(bar)
                ImportProgress.Phase.Scanning -> LinearProgressIndicator(bar)
                ImportProgress.Phase.Matching ->
                    LinearProgressIndicator(progress = { progress.settled.toFloat() / progress.items.coerceAtLeast(1) }, modifier = bar)
                else -> Unit
            }
            if (progress.offline || progress.unreachable > 0) {
                Text(
                    if (progress.offline) stringResource(R.string.import_offline)
                    else pluralStringResource(R.plurals.import_unreachable_items, progress.unreachable, progress.unreachable),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(top = 12.dp, end = 8.dp),
                )
                FlowRow {
                    if (progress.unreachable > 0) {
                        TextButton(onClick = importer::retryUnreachable) { Text(stringResource(R.string.import_retry_unreachable)) }
                    }
                    TextButton(onClick = importer::skipTagging) { Text(stringResource(R.string.import_skip_tagging)) }
                }
            }
            for (problem in progress.problems.take(MAX_PROBLEMS)) {
                Text(
                    problem,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = 6.dp, end = 8.dp),
                )
            }
            if (progress.problems.size > MAX_PROBLEMS) {
                val more = progress.problems.size - MAX_PROBLEMS
                Text(
                    pluralStringResource(R.plurals.import_more_problems, more, more),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(top = 2.dp),
                )
            }
            for (outcome in progress.outcomes.take(MAX_PROBLEMS)) {
                Text(
                    outcome,
                    style = MaterialTheme.typography.bodySmall,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = 6.dp, end = 8.dp),
                )
            }
            if (progress.outcomes.size > MAX_PROBLEMS) {
                val more = progress.outcomes.size - MAX_PROBLEMS
                Text(
                    pluralStringResource(R.plurals.import_more_problems, more, more),
                    style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.padding(top = 2.dp),
                )
            }
            FlowRow(
                Modifier.fillMaxWidth().padding(vertical = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(4.dp, Alignment.End),
            ) {
                if (progress.review > 0) {
                    TextButton(onClick = onReview) { Text(stringResource(R.string.import_review_open)) }
                }
                when (progress.phase) {
                    ImportProgress.Phase.Copying, ImportProgress.Phase.Scanning, ImportProgress.Phase.Matching ->
                        TextButton(onClick = importer::cancel) { Text(stringResource(R.string.cancel)) }
                    ImportProgress.Phase.Review -> {
                        TextButton(onClick = importer::skipAll) { Text(stringResource(R.string.import_skip_all)) }
                        TextButton(onClick = importer::acceptAllAsIs) { Text(stringResource(R.string.import_tags_all)) }
                    }
                    ImportProgress.Phase.Finished, ImportProgress.Phase.Cancelled ->
                        TextButton(onClick = importer::dismiss) { Text(stringResource(R.string.done)) }
                }
            }
        }
    }
}

private const val MAX_PROBLEMS = 3
