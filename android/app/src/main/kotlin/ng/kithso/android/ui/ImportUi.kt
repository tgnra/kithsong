package ng.kithso.android.ui

import android.content.ActivityNotFoundException
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
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
import ng.kithso.android.importer.ItemKey
import ng.kithso.android.importer.ReviewDetails
import ng.kithso.android.importer.TrackPick
import ng.kithso.android.importer.detail
import ng.kithso.android.importer.title
import ng.kithso.core.CoreImport

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
 * with no sure match, to review one by one or settle all at once; then a summary with what
 * failed and what the library already had, until dismissed.
 */
@Composable
fun ImportCard(progress: ImportProgress, importer: Importer, modifier: Modifier = Modifier) {
    var reviewing by rememberSaveable { mutableStateOf(false) }
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
                    TextButton(onClick = { reviewing = true }) { Text(stringResource(R.string.import_review_open)) }
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
    if (reviewing) ReviewSheet(progress, importer, onClose = { reviewing = false })
}

private const val MAX_PROBLEMS = 3

/**
 * The items waiting for a decision, and one of them opened: its candidates on MusicBrainz, each
 * opening to its tracks to import it with ([MatchPage]), its existing tags, skipping it,
 * searching again with the user's own terms and, for an album, searching more widely. It closes
 * when nothing is left to decide.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ReviewSheet(progress: ImportProgress, importer: Importer, onClose: () -> Unit) {
    var open by remember { mutableStateOf<ItemKey?>(null) }
    val items = progress.reviewItems
    LaunchedEffect(items.isEmpty(), open) {
        if (items.isEmpty() && open == null) onClose()
    }
    ModalBottomSheet(onDismissRequest = onClose, sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)) {
        Column(Modifier.navigationBarsPadding()) {
            val key = open
            if (key == null) {
                Text(
                    stringResource(R.string.import_review_title),
                    style = MaterialTheme.typography.titleLarge,
                    modifier = Modifier.padding(horizontal = 24.dp),
                )
                if (items.isEmpty()) {
                    Text(
                        stringResource(R.string.import_review_empty),
                        style = MaterialTheme.typography.bodyLarge,
                        modifier = Modifier.padding(24.dp),
                    )
                } else {
                    LazyColumn(Modifier.heightIn(max = 480.dp)) {
                        items(items, key = { "${it.key.batch}/${it.key.item}" }) { entry ->
                            ListItem(
                                headlineContent = { Text(entry.name, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                                supportingContent = entry.reason?.let { { Text(it) } },
                                colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                                modifier = Modifier.clickable { open = entry.key },
                            )
                        }
                    }
                }
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
                ) {
                    TextButton(onClick = onClose) { Text(stringResource(R.string.done)) }
                }
            } else {
                ReviewItem(key, progress, importer, onBack = { open = null })
            }
        }
    }
}

@Composable
private fun ReviewItem(key: ItemKey, progress: ImportProgress, importer: Importer, onBack: () -> Unit) {
    var details by remember(key) { mutableStateOf<ReviewDetails?>(null) }
    var loaded by remember(key) { mutableStateOf(false) }
    var searching by remember(key) { mutableStateOf(false) }
    var chosen by remember(key) { mutableStateOf<Int?>(null) }
    // Read again as the import moves on: a search adds candidates, a decision settles it.
    LaunchedEffect(key, progress) {
        details = importer.details(key)
        loaded = true
    }
    val d = details
    val state = d?.info?.state
    LaunchedEffect(loaded, state) {
        if (loaded && (d == null || state?.isFinished == true)) onBack()
    }
    Column(Modifier.padding(horizontal = 24.dp)) {
        if (d == null) {
            Box(Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
            return@Column
        }
        Text(d.name, style = MaterialTheme.typography.titleLarge)
        val single = d.info.kind == CoreImport.Kind.Single
        Text(
            if (single) stringResource(R.string.import_single)
            else pluralStringResource(R.plurals.import_files_count, d.info.fileCount, d.info.fileCount),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (state?.isBusy == true) {
            Row(Modifier.padding(vertical = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                CircularProgressIndicator(Modifier.padding(end = 12.dp))
                Text(stringResource(R.string.import_searching))
            }
        } else {
            d.info.error?.let {
                Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(top = 8.dp))
            }
        }
    }
    if (d == null) return
    val busy = d.info.state.isBusy
    d.matches.getOrNull(chosen ?: -1)?.let { match ->
        MatchPage(d, chosen ?: 0, match, busy, importer, onBack = { chosen = null })
        return
    }
    if (d.matches.isNotEmpty()) {
        Text(
            stringResource(R.string.import_matches),
            style = MaterialTheme.typography.titleSmall,
            modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 16.dp),
        )
        LazyColumn(Modifier.heightIn(max = 320.dp)) {
            items(d.matches.size) { i ->
                val m = d.matches[i]
                // A single by its track's title, with the release it's filed on among the facts.
                val title = if (m.isSingle) m.track.ifEmpty { m.album } else m.album
                val heading = if (m.year > 0) stringResource(R.string.import_match_year, m.albumArtist, title, m.year)
                else stringResource(R.string.track_byline, m.albumArtist, title)
                val facts = listOfNotNull(
                    m.release.takeIf { m.isSingle && it.isNotEmpty() && it != title },
                    stringResource(R.string.import_match_details, (m.confidence * 100).toInt()),
                    pluralStringResource(R.plurals.import_match_tracks, m.trackCount, m.trackCount).takeIf { m.trackCount > 0 },
                    m.format.ifEmpty { null },
                    m.country.ifEmpty { null },
                    m.label.ifEmpty { null },
                ).joinToString(" · ")
                ListItem(
                    headlineContent = { Text(heading, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                    supportingContent = { Text(facts, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                    modifier = Modifier.clickable(enabled = !busy) { chosen = i },
                )
            }
        }
        HorizontalDivider()
    }
    FlowRow(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
    ) {
        TextButton(onClick = onBack) { Text(stringResource(R.string.back)) }
        // Offline it would only wait (see mc_import_search_wider).
        if (!progress.offline) {
            OutlinedButton(onClick = { importer.searchWider(key) }, enabled = !busy) { Text(stringResource(R.string.import_search_wider)) }
        }
        OutlinedButton(onClick = { searching = true }, enabled = !busy) { Text(stringResource(R.string.import_search_again)) }
        OutlinedButton(onClick = { importer.skip(key) }, enabled = !busy) { Text(stringResource(R.string.import_skip)) }
        Button(onClick = { importer.acceptAsIs(key) }, enabled = !busy) { Text(stringResource(R.string.import_as_tagged)) }
    }
    if (searching) {
        SearchDialog(
            artist = d.info.artist,
            title = d.info.album,
            onSearch = { artist, title ->
                searching = false
                importer.search(key, artist, title)
            },
            onDismiss = { searching = false },
        )
    }
}

// A file that can be made a missing track: [group] says which other item it's from, if one.
private data class Candidate(val fromItem: Int, val file: CoreImport.ItemFile, val group: String?)

private val CoreImport.ItemFile.name: String get() = path.substringAfterLast('/')

/**
 * A match opened: its tracks with the files they'd be applied to, then the files left over and,
 * with tracks missing, those of other items from the folder. Tapping a missing track lists the
 * files it can take, and tapping it again takes the file back; Use this match imports the item
 * with them. They're picked first and applied together, as an item is written with the first.
 */
@Composable
private fun MatchPage(d: ReviewDetails, index: Int, match: CoreImport.Match, busy: Boolean, importer: Importer, onBack: () -> Unit) {
    val picks = remember(d.key, index) { mutableStateMapOf<Int, Candidate>() }
    val used = match.tracks.mapNotNull { t -> t.file.takeIf { it >= 0 } }.toSet()
    val own = d.files.filterIndexed { i, _ -> i !in used }
    val hasMissing = match.tracks.any { it.file < 0 }
    val album = d.info.kind == CoreImport.Kind.Album
    val otherText = stringResource(R.string.import_not_matched)
    val related = if (hasMissing && album) {
        d.related.flatMap { other ->
            val group = stringResource(R.string.import_other_item_file, other.name)
            other.files.map { Candidate(other.item, it, group) }
        }
    } else {
        emptyList()
    }
    val candidates = own.map { Candidate(d.key.item, it, null) } + related
    val picked = picks.values.map { it.file.path }.toSet()
    val left = candidates.filter { it.file.path !in picked }
    val canAssign = album && hasMissing && candidates.isNotEmpty()
    val numbered = (match.tracks.lastOrNull()?.disc ?: 0) > 1
    Text(
        stringResource(R.string.import_tracks_text),
        style = MaterialTheme.typography.titleSmall,
        modifier = Modifier.padding(start = 24.dp, end = 24.dp, top = 16.dp),
    )
    if (canAssign) {
        Text(
            stringResource(R.string.import_use_as_track_hint),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(start = 24.dp, end = 24.dp, bottom = 8.dp),
        )
    }
    LazyColumn(Modifier.heightIn(max = 400.dp)) {
        items(match.tracks.size) { i ->
            val t = match.tracks[i]
            val number = if (numbered) "${t.disc}-${t.number}" else t.number.toString()
            MatchTrackRow(
                title = if (album) "$number. ${t.title}" else t.title,
                file = d.files.getOrNull(t.file)?.name ?: picks[i]?.file?.name,
                lengthMs = t.lengthMs,
                useAs = stringResource(R.string.import_use_as_track, number),
                choices = if (canAssign && t.file < 0 && !busy) left else emptyList(),
                onPick = { picks[i] = it },
                onUnpick = if (picks.containsKey(i)) ({ picks.remove(i) }) else null,
            )
        }
        items(candidates.filter { it.file.path !in picked }) { c ->
            ListItem(
                headlineContent = { Text(c.file.name, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                supportingContent = { Text(c.group ?: otherText) },
                trailingContent = { Text(formatTime(c.file.durationMs.toLong())) },
                colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            )
        }
    }
    HorizontalDivider()
    FlowRow(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
    ) {
        TextButton(onClick = onBack) { Text(stringResource(R.string.back)) }
        Button(
            onClick = {
                importer.accept(d.key, index, picks.map { (track, c) -> TrackPick(track, c.fromItem, c.file.path) })
            },
            enabled = !busy,
        ) { Text(stringResource(R.string.import_use_match)) }
    }
}

// A track of a match, and a missing one's menu of [choices].
@Composable
private fun MatchTrackRow(
    title: String,
    file: String?,
    lengthMs: Int,
    useAs: String,
    choices: List<Candidate>,
    onPick: (Candidate) -> Unit,
    onUnpick: (() -> Unit)?,
) {
    var menu by remember { mutableStateOf(false) }
    Box {
        ListItem(
            headlineContent = { Text(title, maxLines = 2, overflow = TextOverflow.Ellipsis) },
            supportingContent = { Text(file ?: stringResource(R.string.import_track_missing), maxLines = 1, overflow = TextOverflow.Ellipsis) },
            trailingContent = { Text(formatTime(lengthMs.toLong())) },
            colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            modifier = Modifier.clickable(enabled = onUnpick != null || choices.isNotEmpty()) {
                if (onUnpick != null) onUnpick() else menu = true
            },
        )
        DropdownMenu(expanded = menu, onDismissRequest = { menu = false }) {
            Text(
                useAs,
                style = MaterialTheme.typography.titleSmall,
                modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
            )
            for (c in choices) {
                DropdownMenuItem(
                    text = {
                        Column {
                            Text(c.file.name, maxLines = 2, overflow = TextOverflow.Ellipsis)
                            c.group?.let { Text(it, style = MaterialTheme.typography.bodySmall) }
                        }
                    },
                    onClick = {
                        menu = false
                        onPick(c)
                    },
                )
            }
        }
    }
}

@Composable
private fun SearchDialog(artist: String, title: String, onSearch: (String, String) -> Unit, onDismiss: () -> Unit) {
    var a by rememberSaveable { mutableStateOf(artist) }
    var t by rememberSaveable { mutableStateOf(title) }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.import_search_again)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedTextField(a, { a = it }, label = { Text(stringResource(R.string.import_search_artist)) }, singleLine = true)
                OutlinedTextField(t, { t = it }, label = { Text(stringResource(R.string.import_search_title)) }, singleLine = true)
            }
        },
        confirmButton = {
            TextButton(onClick = { onSearch(a, t) }, enabled = a.isNotBlank() || t.isNotBlank()) {
                Text(stringResource(R.string.search))
            }
        },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}
