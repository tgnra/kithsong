package ng.kithso.android.ui

import android.util.Log
import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
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
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.Saver
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.TagEditor
import ng.kithso.android.data.TagResult
import ng.kithso.android.data.TagTarget
import ng.kithso.android.data.TagValues
import ng.kithso.android.playback.Playback
import ng.kithso.core.AlbumFlags
import ng.kithso.core.AlbumTags
import ng.kithso.core.ArtistTags
import ng.kithso.core.CoreEdit
import ng.kithso.core.ReleaseInfo
import ng.kithso.core.TrackTags
import kotlinx.coroutines.launch
import kotlin.math.roundToInt

/** The form's text boxes, by what they hold. */
internal enum class TagField { Title, Name, Artist, SortName, TrackNo, DiscNo, Year, OriginalYear, Genre, Release }

private const val TAG = "kithsong"
private const val YEAR_DIGITS = 4
private const val NUMBER_DIGITS = 3

/**
 * The tag editor's form: its text as typed, an album's flags, and [saved], what the library has,
 * null until it's been read (or if it couldn't be). What's typed is kept through a rotation, so
 * [restored] is the text and flags [saver] kept, which win over what's read in.
 */
@Stable
internal class TagForm(val target: TagTarget, private val restored: List<String>? = null) {
    private val text = mutableStateMapOf<TagField, String>()
    var noScrobble by mutableStateOf(false)
    var mix by mutableStateOf(false)
    var saved by mutableStateOf<TagValues?>(null)
    var saving by mutableStateOf(false)

    /** What the form has to say, and if [failed], it's that something went wrong. */
    var message by mutableStateOf<String?>(null)
    var failed by mutableStateOf(false)

    /** Save found a file in use while something plays: Stop playing and retry shows. */
    var inUse by mutableStateOf(false)

    init {
        if (restored != null) {
            TagField.entries.forEachIndexed { i, field -> text[field] = restored[i] }
            noScrobble = restored[TagField.entries.size].toBoolean()
            mix = restored[TagField.entries.size + 1].toBoolean()
        }
    }

    operator fun get(field: TagField): String = text[field].orEmpty()

    operator fun set(field: TagField, value: String) {
        text[field] = value
    }

    /** Whether there's something to edit and it can be edited now. */
    val editable: Boolean get() = saved != null && !saving

    /** The library's [values], read in: what the form starts from, unless it was [restored]. */
    fun start(values: TagValues) {
        saved = values
        if (restored != null) return
        when (values) {
            is TagValues.Album -> {
                fill(values.tags)
                noScrobble = values.flags.noScrobble
                mix = values.flags.mix
            }
            is TagValues.Artist -> {
                this[TagField.Name] = values.tags.name
                this[TagField.SortName] = values.tags.sortName
            }
            is TagValues.Track -> {
                this[TagField.Title] = values.tags.title
                this[TagField.Artist] = values.tags.artist
                this[TagField.TrackNo] = numberText(values.tags.trackNo)
                this[TagField.DiscNo] = numberText(values.tags.discNo)
                this[TagField.Year] = numberText(values.tags.year)
                this[TagField.Genre] = values.tags.genre
            }
        }
    }

    /** Fills in an album's tags: as read in, or a release's, for Save to keep. */
    fun fill(tags: AlbumTags) {
        this[TagField.Title] = tags.title
        this[TagField.Artist] = tags.artist
        this[TagField.Year] = numberText(tags.year)
        this[TagField.OriginalYear] = numberText(tags.originalYear)
        this[TagField.Release] = tags.mbAlbumId
    }

    /** What the form says, as the library would have it: 007 is 7, and nothing is 0, unknown. */
    fun values(): TagValues? = when (saved) {
        null -> null
        is TagValues.Album -> TagValues.Album(
            AlbumTags(
                this[TagField.Title], this[TagField.Artist], number(TagField.Year), number(TagField.OriginalYear),
                this[TagField.Release],
            ),
            AlbumFlags(noScrobble, mix),
        )
        is TagValues.Artist -> TagValues.Artist(ArtistTags(this[TagField.Name], this[TagField.SortName]))
        is TagValues.Track -> TagValues.Track(
            TrackTags(
                this[TagField.Title], this[TagField.Artist], number(TagField.TrackNo), number(TagField.DiscNo),
                number(TagField.Year), this[TagField.Genre],
            ),
        )
    }

    /** Whether leaving now would lose something. */
    val changed: Boolean get() = saved != null && values() != saved

    private fun number(field: TagField) = this[field].toIntOrNull() ?: 0

    private fun numberText(n: Int) = if (n != 0) n.toString() else ""

    companion object {
        /** Keeps the text and flags once they've been read in; what the library has is read again. */
        fun saver(target: TagTarget): Saver<TagForm, Any> = listSaver(
            save = { form ->
                if (form.saved == null) emptyList()
                else TagField.entries.map { form[it] } + form.noScrobble.toString() + form.mix.toString()
            },
            restore = { TagForm(target, it.takeIf(List<String>::isNotEmpty)) },
        )
    }
}

/**
 * The tag editor, ported from Windows' (edit_dialog.c), over everything as a layer of its own: a
 * page rather than a dialog, with Cancel, the title and Save along the top, for an album its
 * title, artist, years, MusicBrainz release and the two options, for an artist its name and sort
 * name, and for a track its title, artist, numbers, year and genre. Nothing changes until Save.
 *
 * Search MusicBrainz… opens a page over this one ([ReleaseSearchPage]), where using a release
 * fills in the form, MusicBrainz release included. Save calls only what changed, off the main
 * thread ([TagEditor.save]); nothing to save just closes. It can't be cancelled once it's
 * started, so back and Cancel wait for it, and the page stays open and says why if it fails.
 * Back or Cancel with something changed asks first; the page doesn't swipe away.
 *
 * Android lets a file be replaced while it's open, so the track playing plays on from its old
 * file whatever the edit does, and the core reports a file in use only if the files kept
 * changing under it (the Stop playing and retry that follows is then Windows' own). But a queue
 * knows its tracks by path, and an edit can move them, so once saved [playback]'s queue is
 * given their new paths and titles, which restarts the track from where it was
 * ([Playback.updateQueue]); one that isn't any different is left alone. [onClose] closes the editor.
 */
@Composable
internal fun TagEditorScreen(editor: TagEditor, playback: Playback, target: TagTarget, onClose: () -> Unit) {
    val form = rememberSaveable(target, saver = TagForm.saver(target)) { TagForm(target) }
    var searching by rememberSaveable(target) { mutableStateOf(false) }
    var asking by rememberSaveable(target) { mutableStateOf(false) }
    val context = LocalContext.current
    LaunchedEffect(form) {
        when (val read = editor.read(target)) {
            is TagResult.Ok -> form.start(read.value)
            // An empty form with only Cancel to press, saying why.
            is TagResult.Failed -> {
                form.failed = true
                form.message = if (read.status == CoreEdit.ERR_NOT_FOUND) context.getString(R.string.edit_gone)
                else context.getString(R.string.edit_load_failed, CoreEdit.statusText(read.status))
            }
        }
    }
    Box(Modifier.fillMaxSize()) {
        DismissibleScreen(
            DismissDirection.Right,
            onDismissed = onClose,
            modifier = if (searching) Modifier.clearAndSetSemantics {} else Modifier,
            backEnabled = !searching && !form.saving && !form.changed,
            // A stray swipe would lose the edit: only back or Cancel close it.
            draggable = false,
        ) { dismiss ->
            val leave = { if (form.changed) asking = true else dismiss() }
            BackHandler(enabled = !searching && (form.saving || form.changed)) { if (!form.saving) leave() }
            TagPage(editor, playback, form, onLeave = { leave() }, onSaved = { dismiss() }, onSearch = { searching = true })
            if (asking) {
                AlertDialog(
                    onDismissRequest = { asking = false },
                    title = { Text(stringResource(R.string.edit_discard_title)) },
                    confirmButton = {
                        TextButton(onClick = {
                            asking = false
                            dismiss()
                        }) {
                            Text(stringResource(R.string.edit_discard), color = MaterialTheme.colorScheme.error)
                        }
                    },
                    dismissButton = {
                        TextButton(onClick = { asking = false }) { Text(stringResource(R.string.edit_keep_editing)) }
                    },
                )
            }
        }
        if (searching) {
            DismissibleScreen(DismissDirection.Right, onDismissed = { searching = false }) { dismiss ->
                ReleaseSearchPage(
                    editor,
                    target.uid,
                    form[TagField.Artist],
                    form[TagField.Title],
                    onBack = { dismiss() },
                    onUse = { tags ->
                        form.fill(tags)
                        form.message = null
                        dismiss()
                    },
                )
            }
        }
    }
}

/** A page of the editor: [start] (Cancel or back), [title] and [end] along the top, over [content]. */
@Composable
private fun TagFrame(
    title: String,
    start: @Composable () -> Unit,
    end: @Composable () -> Unit,
    content: @Composable ColumnScope.() -> Unit,
) {
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        Column(
            Modifier
                .fillMaxSize()
                .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal))
                // Whichever is taller: the keyboard stands on the navigation bar's space.
                .windowInsetsPadding(WindowInsets.ime.union(WindowInsets.navigationBars).only(WindowInsetsSides.Bottom)),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Row(Modifier.fillMaxWidth().statusBarsPadding().padding(4.dp), verticalAlignment = Alignment.CenterVertically) {
                start()
                Text(
                    title,
                    style = MaterialTheme.typography.titleLarge,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f).padding(horizontal = 8.dp).semantics { heading() },
                )
                end()
            }
            content()
        }
    }
}

@Composable
private fun TagPage(
    editor: TagEditor,
    playback: Playback,
    form: TagForm,
    onLeave: () -> Unit,
    onSaved: () -> Unit,
    onSearch: () -> Unit,
) {
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val focus = LocalFocusManager.current
    val save: () -> Unit = save@{
        val was = form.saved ?: return@save
        val now = form.values() ?: return@save
        if (form.saving) return@save
        focus.clearFocus()
        if (now == was) {
            onSaved() // there's nothing to lose
            return@save
        }
        form.saving = true
        form.inUse = false
        form.failed = false
        form.message = context.getString(R.string.edit_saving)
        scope.launch {
            val result = editor.save(form.target, was, now)
            form.saved = result.saved // what did get saved is saved: a second try leaves it out
            if (result.saved != was) {
                val queue = playback.queueTracks
                val fresh = if (queue.isEmpty()) queue else editor.freshTracks(queue)
                if (fresh != queue) {
                    Log.i(TAG, "The edit changed the queue's tracks: giving the player the new ones")
                    playback.updateQueue(fresh)
                }
            }
            form.saving = false
            if (result.status == 0) {
                form.message = null
                onSaved()
            } else {
                form.failed = true
                form.message = if (result.status == CoreEdit.ERR_NOT_FOUND) context.getString(R.string.edit_gone)
                else context.getString(R.string.edit_save_failed, CoreEdit.statusText(result.status))
                // Nothing changed, and it may yet be the track playing.
                form.inUse = result.status == CoreEdit.ERR_IN_USE && playback.queueUids.isNotEmpty()
            }
        }
    }
    val title = when (form.target.kind) {
        TagTarget.Kind.Album -> R.string.edit_album_title
        TagTarget.Kind.Artist -> R.string.edit_artist_title
        TagTarget.Kind.Track -> R.string.edit_track_title
    }
    TagFrame(
        stringResource(title),
        start = {
            TextButton(onClick = onLeave, enabled = !form.saving) { Text(stringResource(R.string.cancel)) }
        },
        end = {
            TextButton(onClick = save, enabled = form.editable) { Text(stringResource(R.string.save)) }
        },
    ) {
        if (form.saving) LinearProgressIndicator(Modifier.fillMaxWidth())
        form.message?.let { message ->
            Column(Modifier.fillMaxWidth().widthIn(max = MaxFormWidth).padding(horizontal = 16.dp, vertical = 8.dp)) {
                Text(
                    message,
                    color = if (form.failed) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Assertive },
                )
                if (form.inUse) {
                    OutlinedButton(
                        onClick = {
                            playback.clear() // lets go of the file now, so the save can have it
                            save()
                        },
                        modifier = Modifier.padding(top = 8.dp),
                    ) {
                        Text(stringResource(R.string.edit_stop_retry))
                    }
                }
            }
        }
        Column(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .verticalScroll(rememberScrollState()),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Column(
                Modifier.widthIn(max = MaxFormWidth).fillMaxWidth().padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                when (form.target.kind) {
                    TagTarget.Kind.Album -> AlbumFields(form, save, onSearch)
                    TagTarget.Kind.Artist -> ArtistFields(form, save)
                    TagTarget.Kind.Track -> TrackFields(form, save)
                }
            }
        }
    }
}

@Composable
private fun AlbumFields(form: TagForm, onDone: () -> Unit, onSearch: () -> Unit) {
    TagTextField(form, TagField.Title, stringResource(R.string.title))
    TagTextField(form, TagField.Artist, stringResource(R.string.edit_album_artist))
    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        TagTextField(form, TagField.Year, stringResource(R.string.col_year), Modifier.weight(1f), digits = YEAR_DIGITS)
        TagTextField(
            form, TagField.OriginalYear, stringResource(R.string.edit_original_year), Modifier.weight(1f),
            digits = YEAR_DIGITS, onDone = onDone,
        )
    }
    // Never typed in, only searched for.
    OutlinedTextField(
        value = form[TagField.Release].ifEmpty { stringResource(R.string.edit_mb_none) },
        onValueChange = {},
        readOnly = true,
        enabled = form.editable,
        singleLine = true,
        label = { Text(stringResource(R.string.edit_mb_release)) },
        modifier = Modifier.fillMaxWidth(),
    )
    OutlinedButton(onClick = onSearch, enabled = form.editable, modifier = Modifier.fillMaxWidth()) {
        Text(stringResource(R.string.edit_mb_search))
    }
    Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer)) {
        TagOption(
            stringResource(R.string.edit_no_scrobble), stringResource(R.string.edit_no_scrobble_text),
            form.noScrobble, form.editable,
        ) { form.noScrobble = it }
        TagOption(stringResource(R.string.edit_mix), stringResource(R.string.edit_mix_text), form.mix, form.editable) {
            form.mix = it
        }
    }
}

@Composable
private fun ArtistFields(form: TagForm, onDone: () -> Unit) {
    TagTextField(form, TagField.Name, stringResource(R.string.name))
    TagTextField(form, TagField.SortName, stringResource(R.string.edit_sort_name), onDone = onDone)
}

@Composable
private fun TrackFields(form: TagForm, onDone: () -> Unit) {
    TagTextField(form, TagField.Title, stringResource(R.string.title))
    TagTextField(form, TagField.Artist, stringResource(R.string.artist), cue = stringResource(R.string.edit_track_artist_cue))
    // Two to a row: three leave a phone no room for the labels.
    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        TagTextField(form, TagField.TrackNo, stringResource(R.string.edit_track_number), Modifier.weight(1f), digits = NUMBER_DIGITS)
        TagTextField(form, TagField.DiscNo, stringResource(R.string.edit_disc_number), Modifier.weight(1f), digits = NUMBER_DIGITS)
    }
    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        TagTextField(form, TagField.Year, stringResource(R.string.col_year), Modifier.weight(1f), digits = YEAR_DIGITS)
        TagTextField(form, TagField.Genre, stringResource(R.string.edit_genre), Modifier.weight(1f), onDone = onDone)
    }
}

/**
 * A text box of [form]'s. [digits]: a number of at most that many digits. [cue] says under an
 * empty box what that means. With [onDone], it's the form's last: the keyboard's action saves,
 * as Enter does on Windows; the others go on to the next box.
 */
@Composable
private fun TagTextField(
    form: TagForm,
    field: TagField,
    label: String,
    modifier: Modifier = Modifier.fillMaxWidth(),
    digits: Int = 0,
    cue: String? = null,
    onDone: (() -> Unit)? = null,
) {
    OutlinedTextField(
        value = form[field],
        onValueChange = { value -> form[field] = if (digits > 0) value.filter { it in '0'..'9' }.take(digits) else value },
        enabled = form.editable,
        singleLine = true,
        label = { Text(label, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingText = if (cue != null && form[field].isEmpty()) ({ Text(cue) }) else null,
        keyboardOptions = KeyboardOptions(
            capitalization = if (digits > 0) KeyboardCapitalization.None else KeyboardCapitalization.Sentences,
            keyboardType = if (digits > 0) KeyboardType.Number else KeyboardType.Text,
            imeAction = if (onDone != null) ImeAction.Done else ImeAction.Next,
        ),
        keyboardActions = KeyboardActions(onDone = { onDone?.invoke() }),
        modifier = modifier,
    )
}

/** A check with its name and what it does: the whole row toggles it. */
@Composable
private fun TagOption(name: String, note: String, checked: Boolean, enabled: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .toggleable(checked, enabled = enabled, role = Role.Checkbox, onValueChange = onChange)
            .padding(12.dp),
    ) {
        Checkbox(checked, onCheckedChange = null, enabled = enabled)
        Spacer(Modifier.width(12.dp))
        Column {
            Text(name, style = MaterialTheme.typography.bodyLarge)
            Text(note, style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

/**
 * Search MusicBrainz…: artist and title boxes, filled in from the form, and the releases found,
 * best fit first and the first picked. It searches as it opens, as that's what was asked for, if
 * there's a title to search for. [onUse] gets what the album's tags would be on the release picked.
 * Closing it leaves a search running by itself; there's no stopping a network call.
 */
@Composable
private fun ReleaseSearchPage(
    editor: TagEditor,
    albumUid: String,
    formArtist: String,
    formTitle: String,
    onBack: () -> Unit,
    onUse: (AlbumTags) -> Unit,
) {
    var artist by rememberSaveable { mutableStateOf(formArtist) }
    var title by rememberSaveable { mutableStateOf(formTitle) }
    var releases by remember { mutableStateOf<List<ReleaseInfo>>(emptyList()) }
    var picked by remember { mutableIntStateOf(-1) }
    var busy by remember { mutableStateOf(false) }
    var message by remember { mutableStateOf<String?>(null) }
    var failed by remember { mutableStateOf(false) }
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val focus = LocalFocusManager.current
    val unreachable = stringResource(R.string.import_error_unreachable)
    val search: () -> Unit = search@{
        if (busy || title.isEmpty()) return@search
        focus.clearFocus()
        busy = true
        failed = false
        releases = emptyList()
        picked = -1
        message = context.getString(R.string.import_searching)
        scope.launch {
            when (val found = editor.searchReleases(albumUid, artist, title)) {
                is TagResult.Ok -> {
                    releases = found.value
                    picked = if (found.value.isEmpty()) -1 else 0
                    message = if (found.value.isEmpty()) context.getString(R.string.edit_search_nothing) else null
                }
                is TagResult.Failed -> {
                    failed = true
                    message = if (found.status == CoreEdit.ERR_NET) unreachable
                    else context.getString(R.string.edit_search_failed, CoreEdit.statusText(found.status))
                }
            }
            busy = false
        }
    }
    val use: () -> Unit = use@{
        val release = releases.getOrNull(picked) ?: return@use
        if (busy) return@use
        busy = true
        failed = false
        message = context.getString(R.string.edit_search_loading)
        scope.launch {
            when (val tags = editor.releaseTags(albumUid, release.mbAlbumId)) {
                is TagResult.Ok -> onUse(tags.value)
                is TagResult.Failed -> {
                    failed = true
                    message = if (tags.status == CoreEdit.ERR_NET) unreachable
                    else context.getString(R.string.edit_search_load_failed, CoreEdit.statusText(tags.status))
                }
            }
            busy = false
        }
    }
    LaunchedEffect(Unit) { search() }
    TagFrame(
        stringResource(R.string.edit_search_title),
        start = {
            IconButton(onClick = onBack) { Icon(Icons.ArrowBack, contentDescription = stringResource(R.string.back)) }
        },
        end = {
            TextButton(onClick = search, enabled = !busy && title.isNotEmpty()) { Text(stringResource(R.string.search)) }
        },
    ) {
        Column(
            Modifier.widthIn(max = MaxFormWidth).fillMaxWidth().padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedTextField(
                value = artist,
                onValueChange = { artist = it },
                enabled = !busy,
                singleLine = true,
                label = { Text(stringResource(R.string.artist)) },
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Sentences, imeAction = ImeAction.Next),
                modifier = Modifier.fillMaxWidth(),
            )
            OutlinedTextField(
                value = title,
                onValueChange = { title = it },
                enabled = !busy,
                singleLine = true,
                label = { Text(stringResource(R.string.title)) },
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Sentences, imeAction = ImeAction.Search),
                keyboardActions = KeyboardActions(onSearch = { search() }),
                modifier = Modifier.fillMaxWidth(),
            )
        }
        message?.let {
            Row(
                Modifier.widthIn(max = MaxFormWidth).fillMaxWidth().padding(horizontal = 16.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (busy) {
                    CircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
                    Spacer(Modifier.width(8.dp))
                }
                Text(
                    it,
                    color = if (failed) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.semantics { liveRegion = LiveRegionMode.Assertive },
                )
            }
        }
        LazyColumn(Modifier.weight(1f).widthIn(max = MaxFormWidth).fillMaxWidth().padding(top = 8.dp)) {
            itemsIndexed(releases, key = { i, release -> "$i:${release.mbAlbumId}" }) { i, release ->
                ReleaseRow(release, selected = i == picked, enabled = !busy) { picked = i }
            }
        }
        Button(
            onClick = use,
            enabled = !busy && picked >= 0,
            modifier = Modifier.widthIn(max = MaxFormWidth).fillMaxWidth().padding(16.dp),
        ) {
            Text(stringResource(R.string.edit_search_use))
        }
    }
}

/** A release found: its title, artist, what else is known of it, and how well it fits. */
@Composable
private fun ReleaseRow(release: ReleaseInfo, selected: Boolean, enabled: Boolean, onSelect: () -> Unit) {
    val tracks = if (release.trackCount > 0) pluralStringResource(R.plurals.tracks, release.trackCount, release.trackCount) else null
    val details = listOfNotNull(
        release.year.takeIf { it != 0 }?.toString(),
        release.country,
        release.label,
        release.format,
        tracks,
    ).joinToString(stringResource(R.string.separator_dot))
    ListItem(
        headlineContent = { Text(release.title, maxLines = 2, overflow = TextOverflow.Ellipsis) },
        supportingContent = {
            Column {
                Text(release.artist, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (details.isNotEmpty()) Text(details, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
        },
        leadingContent = { RadioButton(selected, onClick = null, enabled = enabled) },
        trailingContent = { Text(stringResource(R.string.edit_fit_percent, (release.score * 100).roundToInt())) },
        colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surface),
        modifier = Modifier.selectable(selected, enabled = enabled, role = Role.RadioButton, onClick = onSelect),
    )
}

/** The form's width at most, on a tablet or in landscape. */
private val MaxFormWidth = 640.dp
