package ng.kithso.android.ui

import android.widget.Toast
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.Addition
import ng.kithso.android.data.Album
import ng.kithso.android.data.Artist
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.DeleteOutcome
import ng.kithso.android.data.DeleteSize
import ng.kithso.android.data.Deletion
import ng.kithso.android.data.LibraryDeleter
import ng.kithso.android.data.Playlist
import ng.kithso.android.data.PlaylistChoice
import ng.kithso.android.data.PlaylistEditor
import ng.kithso.android.data.Track
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * The menus and dialogs for playlists, albums, artists and tracks, shown by [PlaylistSheets]
 * over everything: long-pressing a playlist, album, artist or track opens its menu
 * ([playlistMenu], [albumMenu], [artistMenu], [trackMenu]), and Add to playlist
 * ([addToPlaylist]) opens the list of playlists to add to, with New playlist… first. [editor]
 * is null when the library can't be edited, and [deleter] when nothing can be deleted from it
 * (the made-up one): the menus then offer only playing.
 */
@Stable
class PlaylistActions(val editor: PlaylistEditor?, val deleter: LibraryDeleter? = null) {
    internal var sheet by mutableStateOf<Sheet?>(null)
    internal var dialog by mutableStateOf<Dialog?>(null)

    fun playlistMenu(playlist: Playlist) {
        sheet = Sheet.PlaylistMenu(playlist)
    }

    fun albumMenu(album: Album) {
        sheet = Sheet.AlbumMenu(album)
    }

    fun artistMenu(artist: Artist) {
        sheet = Sheet.ArtistMenu(artist)
    }

    /** [removable]: the track is listed from a manual playlist and can be taken out of it. */
    fun trackMenu(track: Track, removable: Boolean) {
        sheet = Sheet.TrackMenu(track, removable && track.entryUid != null)
    }

    fun addToPlaylist(what: Addition) {
        if (editor != null) sheet = Sheet.AddTo(what)
    }

    internal sealed interface Sheet {
        data class PlaylistMenu(val playlist: Playlist) : Sheet
        data class AlbumMenu(val album: Album) : Sheet
        data class ArtistMenu(val artist: Artist) : Sheet
        data class TrackMenu(val track: Track, val removable: Boolean) : Sheet
        data class AddTo(val what: Addition) : Sheet
    }

    internal sealed interface Dialog {
        data class NewPlaylist(val what: Addition) : Dialog
        data class Rename(val uid: String, val name: String) : Dialog
        data class Delete(val uid: String, val name: String) : Dialog
        /** Deleting an album, artist or track from the library, which is asked first. */
        data class DeleteFromLibrary(val what: Deletion) : Dialog
    }
}

/** One row of a menu sheet. */
private class SheetAction(val label: String, val icon: ImageVector, val onClick: () -> Unit)

/**
 * Shows [actions]' open menu or dialog, if any. [onPlay] plays a collection from its start,
 * or shuffled; [onDeleted] is called with a playlist's uid once it's been deleted, to close
 * its page.
 */
@Composable
fun PlaylistSheets(
    actions: PlaylistActions,
    onPlay: (CollectionRef, shuffle: Boolean) -> Unit,
    onDeleted: (String) -> Unit,
) {
    val editor = actions.editor
    val deleter = actions.deleter
    val scope = rememberCoroutineScope()
    when (val sheet = actions.sheet) {
        null -> Unit
        is PlaylistActions.Sheet.PlaylistMenu -> {
            val playlist = sheet.playlist
            val ref = CollectionRef(CollectionRef.Kind.Playlist, playlist.uid)
            // Smart playlists can only be played.
            val editable by produceState(false, playlist.uid) { value = editor?.isEditable(playlist.uid) == true }
            MenuSheet(
                actions,
                playlist.name,
                pluralStringResource(R.plurals.tracks, playlist.trackCount, playlist.trackCount),
                buildList {
                    add(SheetAction(stringResource(R.string.play), Icons.Play) { onPlay(ref, false) })
                    add(SheetAction(stringResource(R.string.shuffle), Icons.Shuffle) { onPlay(ref, true) })
                    if (editable) {
                        add(SheetAction(stringResource(R.string.playlist_rename_menu), Icons.Edit) {
                            actions.dialog = PlaylistActions.Dialog.Rename(playlist.uid, playlist.name)
                        })
                        add(SheetAction(stringResource(R.string.playlist_delete_menu), Icons.Delete) {
                            actions.dialog = PlaylistActions.Dialog.Delete(playlist.uid, playlist.name)
                        })
                    }
                },
            )
        }
        is PlaylistActions.Sheet.AlbumMenu -> {
            val album = sheet.album
            val ref = CollectionRef(CollectionRef.Kind.Album, album.uid)
            MenuSheet(
                actions,
                album.title,
                album.artist,
                buildList {
                    add(SheetAction(stringResource(R.string.play), Icons.Play) { onPlay(ref, false) })
                    add(SheetAction(stringResource(R.string.shuffle), Icons.Shuffle) { onPlay(ref, true) })
                    if (editor != null) {
                        add(SheetAction(stringResource(R.string.add_to_playlist_menu), Icons.PlaylistAdd) {
                            actions.addToPlaylist(Addition.Album(album.uid))
                        })
                    }
                    if (deleter != null) {
                        add(SheetAction(stringResource(R.string.delete_album_menu), Icons.Delete) {
                            actions.dialog = PlaylistActions.Dialog.DeleteFromLibrary(
                                Deletion(Deletion.Kind.Album, album.uid, album.title),
                            )
                        })
                    }
                },
            )
        }
        is PlaylistActions.Sheet.ArtistMenu -> {
            val artist = sheet.artist
            val ref = CollectionRef(CollectionRef.Kind.Artist, artist.uid)
            MenuSheet(
                actions,
                artist.name,
                artist.subtitle(),
                buildList {
                    add(SheetAction(stringResource(R.string.play), Icons.Play) { onPlay(ref, false) })
                    add(SheetAction(stringResource(R.string.shuffle), Icons.Shuffle) { onPlay(ref, true) })
                    if (deleter != null) {
                        add(SheetAction(stringResource(R.string.delete_artist_menu), Icons.Delete) {
                            actions.dialog = PlaylistActions.Dialog.DeleteFromLibrary(
                                Deletion(Deletion.Kind.Artist, artist.uid, artist.name),
                            )
                        })
                    }
                },
            )
        }
        is PlaylistActions.Sheet.TrackMenu -> {
            val track = sheet.track
            MenuSheet(
                actions,
                track.title,
                stringResource(R.string.track_byline, track.artist, track.album),
                buildList {
                    if (editor != null) {
                        add(SheetAction(stringResource(R.string.add_to_playlist_menu), Icons.PlaylistAdd) {
                            actions.addToPlaylist(Addition.Tracks(listOf(track.uid)))
                        })
                    }
                    val entry = track.entryUid
                    if (editor != null && sheet.removable && entry != null) {
                        add(SheetAction(stringResource(R.string.remove_from_playlist), Icons.Remove) { editor.removeEntry(entry) })
                    }
                    if (deleter != null) {
                        val name = track.title.ifEmpty { stringResource(R.string.unknown_value) }
                        add(SheetAction(stringResource(R.string.delete_track_menu), Icons.Delete) {
                            actions.dialog = PlaylistActions.Dialog.DeleteFromLibrary(
                                Deletion(Deletion.Kind.Track, track.uid, name),
                            )
                        })
                    }
                },
            )
        }
        is PlaylistActions.Sheet.AddTo -> if (editor != null) AddToPlaylistSheet(actions, editor, sheet.what)
    }

    when (val dialog = actions.dialog) {
        null -> Unit
        is PlaylistActions.Dialog.NewPlaylist -> NameDialog(
            title = stringResource(R.string.new_playlist),
            initial = "",
            confirm = stringResource(R.string.create),
            onConfirm = { name -> editor?.addToNewPlaylist(name, dialog.what) },
            onDismiss = { actions.dialog = null },
        )
        is PlaylistActions.Dialog.Rename -> NameDialog(
            title = stringResource(R.string.playlist_rename_title),
            initial = dialog.name,
            confirm = stringResource(R.string.rename),
            onConfirm = { name -> if (name != dialog.name) editor?.renamePlaylist(dialog.uid, name) },
            onDismiss = { actions.dialog = null },
        )
        is PlaylistActions.Dialog.Delete -> AlertDialog(
            onDismissRequest = { actions.dialog = null },
            title = { Text(stringResource(R.string.playlist_delete_title, dialog.name)) },
            text = { Text(stringResource(R.string.playlist_delete_text)) },
            confirmButton = {
                TextButton(onClick = {
                    actions.dialog = null
                    editor?.deletePlaylist(dialog.uid)
                    onDeleted(dialog.uid)
                }) {
                    Text(stringResource(R.string.delete), color = MaterialTheme.colorScheme.error)
                }
            },
            dismissButton = {
                TextButton(onClick = { actions.dialog = null }) { Text(stringResource(R.string.cancel)) }
            },
        )
        is PlaylistActions.Dialog.DeleteFromLibrary -> if (deleter != null) {
            DeleteFromLibraryDialog(deleter, dialog.what, scope, onDismiss = { actions.dialog = null })
        }
    }
}

/**
 * Asks before deleting [what] from the library, saying how many files go from this phone for
 * good (there's no trash to restore them from) and that it goes from every device, then
 * deletes it. Delete waits for the count; something that's gone already closes the dialog.
 * Afterwards a toast says if the delete failed, or some tracks stayed; the desktops show a
 * message box.
 */
@Composable
private fun DeleteFromLibraryDialog(
    deleter: LibraryDeleter,
    what: Deletion,
    // Outlives the dialog, which closes as the delete starts, for the toasts after it.
    scope: CoroutineScope,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current.applicationContext
    val failed = stringResource(R.string.delete_failed)
    val resources = context.resources
    val size by produceState<DeleteSize?>(null, what) {
        val found = deleter.deleteSize(what)
        if (found == null) Toast.makeText(context, failed, Toast.LENGTH_LONG).show()
        if (found == null || found.tracks == 0) onDismiss() else value = found
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.playlist_delete_title, what.name)) },
        text = {
            size?.let {
                Text(
                    if (it.files > 0) pluralStringResource(R.plurals.delete_permanently_text, it.files, it.files)
                    else stringResource(R.string.delete_no_files_text),
                )
            }
        },
        confirmButton = {
            TextButton(
                enabled = size != null,
                onClick = {
                    onDismiss()
                    scope.launch {
                        when (val outcome = deleter.delete(what)) {
                            DeleteOutcome.Deleted -> Unit
                            is DeleteOutcome.Kept -> Toast.makeText(
                                context,
                                resources.getQuantityString(R.plurals.delete_kept, outcome.tracks, outcome.tracks),
                                Toast.LENGTH_LONG,
                            ).show()
                            DeleteOutcome.Failed -> Toast.makeText(context, failed, Toast.LENGTH_LONG).show()
                        }
                    }
                },
            ) {
                Text(stringResource(R.string.delete), color = MaterialTheme.colorScheme.error)
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) }
        },
    )
}

/**
 * A bottom sheet with [title] and [subtitle] over [items]. Picking one slides the sheet away,
 * then does it; the action may open another sheet or a dialog.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun MenuSheet(actions: PlaylistActions, title: String, subtitle: String, items: List<SheetAction>) {
    val state = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val scope = rememberCoroutineScope()
    val current = actions.sheet
    // Only if this sheet is still the one open: the action may have opened another.
    val pick: (() -> Unit) -> Unit = { action ->
        scope.launch { state.hide() }.invokeOnCompletion {
            if (actions.sheet == current) actions.sheet = null
        }
        action()
    }
    ModalBottomSheet(onDismissRequest = { if (actions.sheet == current) actions.sheet = null }, sheetState = state) {
        Column(Modifier.navigationBarsPadding().padding(bottom = 8.dp)) {
            SheetTitle(title, subtitle)
            for (item in items) {
                ListItem(
                    headlineContent = { Text(item.label) },
                    leadingContent = { Icon(item.icon, contentDescription = null) },
                    colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
                    modifier = Modifier.clickable { pick(item.onClick) },
                )
            }
        }
    }
}

@Composable
private fun SheetTitle(title: String, subtitle: String?) {
    Column(Modifier.fillMaxWidth().padding(horizontal = 24.dp).padding(bottom = 8.dp)) {
        Text(
            title,
            style = MaterialTheme.typography.titleMedium,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.semantics { heading() },
        )
        if (subtitle != null) {
            Text(
                subtitle,
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

/**
 * The playlists to add [what] to, New playlist… first, which asks for a name. Every manual
 * playlist is listed, those with nothing to play on this phone too. A toast says where it went.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AddToPlaylistSheet(actions: PlaylistActions, editor: PlaylistEditor, what: Addition) {
    val state = rememberModalBottomSheetState()
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val current = actions.sheet
    val choices by produceState<List<PlaylistChoice>?>(null, editor) { value = editor.playlistChoices() }
    val close: (() -> Unit) -> Unit = { then ->
        scope.launch { state.hide() }.invokeOnCompletion {
            if (actions.sheet == current) actions.sheet = null
        }
        then()
    }
    ModalBottomSheet(onDismissRequest = { if (actions.sheet == current) actions.sheet = null }, sheetState = state) {
        SheetTitle(stringResource(R.string.add_to_playlist), null)
        LazyColumn(Modifier.navigationBarsPadding()) {
            item(key = "new") {
                ListItem(
                    headlineContent = { Text(stringResource(R.string.new_playlist_menu)) },
                    leadingContent = { Icon(Icons.Add, contentDescription = null) },
                    colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
                    modifier = Modifier.clickable { close { actions.dialog = PlaylistActions.Dialog.NewPlaylist(what) } },
                )
            }
            items(choices.orEmpty(), key = { it.uid }) { choice ->
                val added = stringResource(R.string.playlist_added, choice.name)
                ListItem(
                    headlineContent = { Text(choice.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    leadingContent = { Icon(Icons.LibraryMusic, contentDescription = null) },
                    colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
                    modifier = Modifier.clickable {
                        close {
                            editor.addToPlaylist(choice.uid, what)
                            Toast.makeText(context, added, Toast.LENGTH_SHORT).show()
                        }
                    },
                )
            }
        }
    }
}

/** Asks for a playlist's name, starting from [initial] (selected), for [onConfirm]. */
@Composable
private fun NameDialog(
    title: String,
    initial: String,
    confirm: String,
    onConfirm: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    var value by remember { mutableStateOf(TextFieldValue(initial, TextRange(0, initial.length))) }
    val name = value.text.trim()
    val focus = remember { FocusRequester() }
    val done = {
        if (name.isNotEmpty()) {
            onDismiss()
            onConfirm(name)
        }
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(title) },
        text = {
            OutlinedTextField(
                value = value,
                onValueChange = { value = it },
                label = { Text(stringResource(R.string.name)) },
                singleLine = true,
                keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Sentences, imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { done() }),
                modifier = Modifier.fillMaxWidth().focusRequester(focus),
            )
        },
        confirmButton = { TextButton(onClick = done, enabled = name.isNotEmpty()) { Text(confirm) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
    LaunchedEffect(Unit) { focus.requestFocus() }
}
