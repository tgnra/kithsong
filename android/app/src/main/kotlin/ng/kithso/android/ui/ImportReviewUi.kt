package ng.kithso.android.ui

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.importer.ImportEntry
import ng.kithso.android.importer.ImportProgress
import ng.kithso.android.importer.Importer
import ng.kithso.android.importer.ItemKey
import ng.kithso.android.importer.ReviewDetails
import ng.kithso.android.importer.TrackPick
import ng.kithso.core.CoreImport

/**
 * The import's items, as the desktops' import list has them: those waiting for a decision first,
 * then those still being matched or written, then what's been imported, skipped or failed while
 * their session is open. Any that isn't busy opens on its own page ([ImportItemScreen]); Skip all
 * and Keep tags for all settle every item waiting at once. It goes once the import is over.
 */
@Composable
fun ImportScreen(importer: Importer, bottomPadding: Dp, onOpen: (ItemKey) -> Unit, onBack: () -> Unit) {
    val progress by importer.progress.collectAsState()
    val p = progress
    val over = p == null || p.phase.isOver
    LaunchedEffect(over) { if (over) onBack() }
    val entries = p?.entries.orEmpty()
    val groups = listOf(
        R.string.import_needs_decision to entries.filter { it.state == CoreImport.State.Review },
        R.string.import_group_active to entries.filter { it.state.isBusy },
        R.string.import_state_done to entries.filter { it.state == CoreImport.State.Done },
        R.string.import_state_skipped to entries.filter { it.state == CoreImport.State.Skipped },
        R.string.import_state_failed to entries.filter { it.state == CoreImport.State.Failed },
    ).filter { it.second.isNotEmpty() }
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            contentPadding = pageSides().add(WindowInsets(bottom = bottomPadding)).asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "header") {
                ScreenHeader(stringResource(R.string.import_review_title), onBack)
                if (p != null && p.phase == ImportProgress.Phase.Review) {
                    FlowRow(
                        Modifier.fillMaxWidth().padding(horizontal = 8.dp),
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                    ) {
                        TextButton(onClick = importer::skipAll) { Text(stringResource(R.string.import_skip_all)) }
                        TextButton(onClick = importer::acceptAllAsIs) { Text(stringResource(R.string.import_tags_all)) }
                    }
                }
            }
            if (entries.isEmpty()) item(key = "empty") {
                Text(
                    stringResource(R.string.import_review_empty),
                    style = MaterialTheme.typography.bodyLarge,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 24.dp),
                )
            }
            for ((title, list) in groups) {
                item(key = "group:$title") { SettingsHeading(stringResource(title)) }
                items(list, key = { "${it.key.batch}/${it.key.item}" }) { entry ->
                    ListItem(
                        headlineContent = { Text(entry.name, maxLines = 2, overflow = TextOverflow.Ellipsis) },
                        supportingContent = { Text(entryStatus(entry), maxLines = 3, overflow = TextOverflow.Ellipsis) },
                        trailingContent = if (entry.state.isBusy) ({ CircularProgressIndicator(Modifier.size(24.dp), strokeWidth = 2.dp) }) else null,
                        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                        modifier = Modifier.selectable(
                            selected = false,
                            enabled = !entry.state.isBusy,
                            role = Role.Button,
                            onClick = { onOpen(entry.key) },
                        ),
                    )
                }
            }
        }
    }
}

// What an item's state comes to, as the desktops' list says it.
@Composable
private fun entryStatus(entry: ImportEntry): String = when (entry.state) {
    CoreImport.State.Review -> entry.reason ?: stringResource(R.string.import_needs_decision)
    CoreImport.State.Pending -> entry.reason ?: stringResource(R.string.import_state_waiting)
    CoreImport.State.Matching -> stringResource(R.string.import_state_matching)
    CoreImport.State.Writing -> stringResource(R.string.import_state_writing)
    CoreImport.State.Done -> stringResource(
        when (entry.outcome) {
            CoreImport.Outcome.Replaced -> R.string.import_state_replaced
            CoreImport.Outcome.Added -> R.string.import_state_added_missing
            else -> R.string.import_state_done
        },
    )
    CoreImport.State.Skipped -> stringResource(
        if (entry.outcome == CoreImport.Outcome.Duplicate) R.string.import_state_duplicate else R.string.import_state_skipped,
    )
    CoreImport.State.Failed -> entry.reason ?: stringResource(R.string.import_state_failed)
}

// A file that can be made a missing track: [group] says which other item it's from, if one.
private data class Candidate(val fromItem: Int, val file: CoreImport.ItemFile, val group: String?)

private val CoreImport.ItemFile.name: String get() = path.substringAfterLast('/')

/** Wide enough for the matches and the chosen one's tracks side by side, as in the desktops' review. */
private val TwoPaneWidth = 720.dp

/**
 * One item of the import, opened from [ImportScreen]: the desktops' review, on a page of its own.
 * At the top, what it is, where it came from and why it waits; then searching MusicBrainz again
 * with the user's own terms, or more widely; its candidates, one of them chosen; and the chosen
 * one's tracks, each with the file it would be applied to (its name, the title it's tagged with
 * now, and its length), then the files left over and, with tracks missing, those of other items
 * from its folder. A missing track takes one of those files from its menu (a tap, or a long
 * press, which every track row has for what it can do), and gives it back from the same menu.
 * At the bottom, Skip, Keep existing tags and Use this match, which applies the chosen files too.
 *
 * An item already imported can be opened as well, while its session is open, to import it again
 * with another match or with files on its missing tracks, which the core does in place; or with
 * the tags it had. On a wide window the candidates and the tracks are side by side.
 */
@Composable
fun ImportItemScreen(importer: Importer, key: ItemKey, bottomPadding: Dp, onBack: () -> Unit) {
    val progress by importer.progress.collectAsState()
    var details by remember(key) { mutableStateOf<ReviewDetails?>(null) }
    var loaded by remember(key) { mutableStateOf(false) }
    // Read again as the import moves on: a search adds candidates, an assign renumbers files.
    LaunchedEffect(key, progress) {
        details = importer.details(key)
        loaded = true
    }
    val d = details
    // Gone with its session, or its files all went to another item.
    val gone = loaded && (d == null || d.files.isEmpty())
    LaunchedEffect(gone) { if (gone) onBack() }
    var chosen by rememberSaveable(key) { mutableStateOf<Int?>(null) }
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        if (d == null) {
            Column(Modifier.fillMaxSize()) {
                ScreenHeader("", onBack)
                Box(Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) { CircularProgressIndicator() }
            }
            return@Surface
        }
        val busy = d.info.state.isBusy
        // As the desktops: the match it was written with, else the best.
        val index = (chosen ?: d.info.chosen.takeIf { it >= 0 } ?: 0).takeIf { it < d.matches.size } ?: -1
        val match = d.matches.getOrNull(index)
        val picks = remember(key, index) { mutableStateMapOf<Int, Candidate>() }
        val offline = progress?.offline == true
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val wide = maxWidth >= TwoPaneWidth
            Column(Modifier.fillMaxSize()) {
                val sides = pageSides().asPaddingValues()
                if (wide) {
                    Column(Modifier.windowInsetsPadding(pageSides())) { ItemHeader(d, busy, onBack) }
                    Row(Modifier.weight(1f).fillMaxWidth()) {
                        LazyColumn(Modifier.weight(1f).fillMaxSize(), contentPadding = sides) {
                            searchSection(d, busy, offline, importer)
                            matchesSection(d, index, busy) { chosen = it }
                        }
                        LazyColumn(Modifier.weight(1.3f).fillMaxSize(), contentPadding = sides) {
                            tracksSection(d, match, picks, busy)
                        }
                    }
                } else {
                    LazyColumn(Modifier.weight(1f).fillMaxWidth(), contentPadding = sides) {
                        item(key = "header") { ItemHeader(d, busy, onBack) }
                        searchSection(d, busy, offline, importer)
                        matchesSection(d, index, busy) { chosen = it }
                        tracksSection(d, match, picks, busy)
                    }
                }
                HorizontalDivider()
                FlowRow(
                    Modifier.fillMaxWidth()
                        .windowInsetsPadding(pageSides())
                        .padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 8.dp + bottomPadding),
                    horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
                ) {
                    // Done, there's nothing to skip: the files are in the library.
                    OutlinedButton(
                        onClick = {
                            importer.skip(key)
                            onBack()
                        },
                        enabled = !busy && d.info.state != CoreImport.State.Done,
                    ) { Text(stringResource(R.string.import_skip)) }
                    OutlinedButton(
                        onClick = {
                            importer.acceptAsIs(key)
                            onBack()
                        },
                        enabled = !busy,
                    ) { Text(stringResource(R.string.import_as_tagged)) }
                    Button(
                        onClick = {
                            importer.accept(key, index, picks.map { (track, c) -> TrackPick(track, c.fromItem, c.file.path) })
                            onBack()
                        },
                        enabled = !busy && match != null,
                    ) { Text(stringResource(R.string.import_use_match)) }
                }
            }
        }
    }
}

// The page's header: the item's name, what it is, its folder, and searching or why it waits.
@Composable
private fun ItemHeader(d: ReviewDetails, busy: Boolean, onBack: () -> Unit) {
    ScreenHeader(d.name, onBack)
    Column(Modifier.padding(horizontal = 16.dp)) {
        Text(
            if (d.info.kind == CoreImport.Kind.Single) stringResource(R.string.import_single)
            else pluralStringResource(R.plurals.import_files_count, d.info.fileCount, d.info.fileCount),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (d.info.folder.isNotEmpty()) {
            Text(
                d.info.folder,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
            )
        }
        if (busy) {
            Row(Modifier.padding(vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                CircularProgressIndicator(Modifier.padding(end = 12.dp).size(24.dp), strokeWidth = 2.dp)
                Text(stringResource(R.string.import_searching))
            }
        } else {
            d.info.error?.let {
                Text(it, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error, modifier = Modifier.padding(top = 8.dp))
            }
        }
    }
}

// Search again with the user's terms, as the desktops' review has under its matches; and more
// widely, while MusicBrainz can be reached (offline it would only wait, see mc_import_search_wider).
private fun LazyListScope.searchSection(d: ReviewDetails, busy: Boolean, offline: Boolean, importer: Importer) {
    item(key = "search") {
        var artist by rememberSaveable(d.key) { mutableStateOf(d.info.artist) }
        var title by rememberSaveable(d.key) { mutableStateOf(d.info.album) }
        val search = { if (title.isNotBlank()) importer.search(d.key, artist, title) }
        SettingsHeading(stringResource(R.string.import_search_again))
        Column(Modifier.padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(
                artist,
                { artist = it },
                label = { Text(stringResource(R.string.import_search_artist)) },
                singleLine = true,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Next),
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                title,
                { title = it },
                label = { Text(stringResource(if (d.info.kind == CoreImport.Kind.Single) R.string.title else R.string.album)) },
                singleLine = true,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                keyboardActions = KeyboardActions(onSearch = { if (!busy) search() }),
                modifier = Modifier.fillMaxWidth(),
            )
            FlowRow(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End)) {
                if (!offline) {
                    TextButton(onClick = { importer.searchWider(d.key) }, enabled = !busy && !d.info.searchedWider) {
                        Text(stringResource(R.string.import_search_wider))
                    }
                }
                OutlinedButton(onClick = search, enabled = !busy && title.isNotBlank()) { Text(stringResource(R.string.search)) }
            }
        }
    }
}

// The candidates, best first, the chosen one marked; choosing one shows its tracks.
private fun LazyListScope.matchesSection(d: ReviewDetails, chosen: Int, busy: Boolean, onChoose: (Int) -> Unit) {
    item(key = "matches") { SettingsHeading(stringResource(R.string.import_matches)) }
    if (d.matches.isEmpty()) item(key = "matches:none") {
        Text(
            stringResource(R.string.import_matches_empty),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
        )
    }
    items(d.matches.size, key = { "match:$it" }) { i ->
        val m = d.matches[i]
        // A single by its track's title, with the release it's filed on among the facts.
        val title = if (m.isSingle) m.track.ifEmpty { m.album } else m.album
        val heading = if (m.year > 0) stringResource(R.string.import_match_year, m.albumArtist, title, m.year)
        else stringResource(R.string.track_byline, m.albumArtist, title)
        val label = listOf(m.label, m.catalog).filter { it.isNotEmpty() }.joinToString(" · ")
        val facts = listOfNotNull(
            m.release.takeIf { m.isSingle && it.isNotEmpty() && it != title },
            stringResource(R.string.import_match_details, (m.confidence * 100).toInt()),
            pluralStringResource(R.plurals.import_match_tracks, m.trackCount, m.trackCount).takeIf { m.trackCount > 0 },
            m.releaseType.ifEmpty { null },
            m.format.ifEmpty { null },
            m.country.ifEmpty { null },
            label.ifEmpty { null },
        ).joinToString(" · ")
        ListItem(
            leadingContent = { RadioButton(selected = i == chosen, onClick = null, enabled = !busy) },
            headlineContent = { Text(heading, maxLines = 2, overflow = TextOverflow.Ellipsis) },
            supportingContent = { Text(facts, maxLines = 3, overflow = TextOverflow.Ellipsis) },
            colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            modifier = Modifier.selectable(selected = i == chosen, enabled = !busy, role = Role.RadioButton) { onChoose(i) },
        )
    }
}

// The chosen match's tracks with the files they'd get, then the files no track took and, with
// tracks missing, other items' files from the folder: what a missing track's menu offers.
private fun LazyListScope.tracksSection(
    d: ReviewDetails,
    match: CoreImport.Match?,
    picks: MutableMap<Int, Candidate>,
    busy: Boolean,
) {
    val album = d.info.kind == CoreImport.Kind.Album
    val tracks = match?.tracks.orEmpty()
    val used = tracks.mapNotNull { t -> t.file.takeIf { it >= 0 } }.toSet()
    val hasMissing = tracks.any { it.file < 0 }
    val own = d.files.filterIndexed { i, _ -> i !in used }.map { Candidate(d.key.item, it, null) }
    item(key = "tracks") {
        SettingsHeading(stringResource(R.string.tracks_title))
    }
    item(key = "tracks:hint") {
        val related = relatedCandidates(d, hasMissing && album)
        if (album && hasMissing && (own + related).isNotEmpty()) {
            Text(
                stringResource(R.string.import_use_as_track_hint),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
            )
        }
    }
    val numbered = (tracks.lastOrNull()?.disc ?: 0) > 1
    items(tracks.size, key = { "track:$it" }) { i ->
        val t = tracks[i]
        val related = relatedCandidates(d, hasMissing && album)
        val picked = picks.values.map { it.file.path }.toSet()
        val left = (own + related).filter { it.file.path !in picked }
        val number = if (numbered) "${t.disc}-${t.number}" else t.number.toString()
        val pick = picks[i]
        val file = d.files.getOrNull(t.file) ?: pick?.file
        val choices = if (album && t.file < 0 && pick == null && !busy) left else emptyList()
        TrackRow(
            title = if (album) "$number. ${t.title}" else t.title,
            trackTitle = t.title,
            lengthMs = t.lengthMs,
            file = file,
            note = pick?.group ?: if (file == null) stringResource(if (choices.isNotEmpty()) R.string.import_choose_file else R.string.import_track_missing) else null,
            noteIsAction = file == null && choices.isNotEmpty(),
            menu = when {
                pick != null && !busy -> { close ->
                    DropdownMenuItem(
                        text = { Text(stringResource(R.string.import_unpick)) },
                        onClick = {
                            close()
                            picks.remove(i)
                        },
                    )
                }
                choices.isNotEmpty() -> { close ->
                    Text(
                        stringResource(R.string.import_use_as_track, number),
                        style = MaterialTheme.typography.titleSmall,
                        modifier = Modifier.padding(horizontal = 12.dp, vertical = 8.dp),
                    )
                    for (c in choices) {
                        DropdownMenuItem(
                            text = {
                                Column {
                                    Text(c.file.name, maxLines = 2, overflow = TextOverflow.Ellipsis)
                                    Text(
                                        listOfNotNull(c.group, formatTime(c.file.durationMs.toLong())).joinToString(" · "),
                                        style = MaterialTheme.typography.bodySmall,
                                    )
                                }
                            },
                            onClick = {
                                close()
                                picks[i] = c
                            },
                        )
                    }
                }
                else -> null
            },
        )
    }
    // Files that no track took: left as they are if this match is applied.
    items(own.size, key = { "file:${own[it].file.path}" }) { i ->
        val c = own[i]
        if (picks.values.none { it.file.path == c.file.path }) {
            TrackRow(
                title = c.file.name,
                trackTitle = null,
                lengthMs = c.file.durationMs,
                file = null,
                note = listOfNotNull(c.file.title.ifEmpty { null }, if (match != null) stringResource(R.string.import_not_matched) else null)
                    .joinToString(" · ").ifEmpty { null },
                noteIsAction = false,
                menu = null,
            )
        }
    }
    if (hasMissing && album) {
        item(key = "related") {
            Column {
                for (c in relatedCandidates(d, true)) {
                    if (picks.values.none { it.file.path == c.file.path }) {
                        TrackRow(c.file.name, null, c.file.durationMs, null, c.group, noteIsAction = false, menu = null)
                    }
                }
            }
        }
    }
}

// Other items' files from the item's folder, under "Grouped separately: <album>".
@Composable
private fun relatedCandidates(d: ReviewDetails, wanted: Boolean): List<Candidate> {
    if (!wanted) return emptyList()
    return d.related.flatMap { other ->
        val group = stringResource(R.string.import_other_item_file, other.name)
        other.files.map { Candidate(other.item, it, group) }
    }
}

/**
 * A row of an item's track list: a track of the chosen match with the [file] it would be applied
 * to (its name, the title it's tagged with now when that's different, and its length), or a file
 * of its own. [note] says what's missing or where the file comes from, in the primary colour when
 * it's what a tap does ([noteIsAction]). [menu] is what can be done with the row, opened by a tap
 * and by a long press; every row has the slot, so an action for any track goes there.
 */
@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun TrackRow(
    title: String,
    trackTitle: String?,
    lengthMs: Int,
    file: CoreImport.ItemFile?,
    note: String?,
    noteIsAction: Boolean,
    menu: (@Composable (close: () -> Unit) -> Unit)?,
) {
    var open by remember { mutableStateOf(false) }
    val lines = listOfNotNull(
        file?.let { f -> listOfNotNull(f.name, f.title.takeIf { it.isNotEmpty() && it != trackTitle }).joinToString(" · ") },
        note,
    )
    Box {
        ListItem(
            headlineContent = { Text(title, maxLines = 2, overflow = TextOverflow.Ellipsis) },
            supportingContent = if (lines.isEmpty()) null else ({
                Column {
                    for ((n, line) in lines.withIndex()) {
                        val action = noteIsAction && n == lines.lastIndex
                        Text(
                            line,
                            maxLines = 2,
                            overflow = TextOverflow.Ellipsis,
                            color = if (action) MaterialTheme.colorScheme.primary else Color.Unspecified,
                        )
                    }
                }
            }),
            trailingContent = {
                Column(horizontalAlignment = Alignment.End) {
                    Text(formatTime(lengthMs.toLong()))
                    // The file's own length, beside the track's, as the desktops' columns have it.
                    if (file != null) {
                        Text(
                            formatTime(file.durationMs.toLong()),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            },
            colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            modifier = Modifier.combinedClickable(
                enabled = menu != null,
                onLongClickLabel = stringResource(R.string.track_menu),
                onLongClick = { open = true },
                onClick = { open = true },
            ),
        )
        if (menu != null) {
            DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
                menu { open = false }
            }
        }
    }
}
