package ng.kithso.android.ui

import android.widget.Toast
import androidx.annotation.StringRes
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
import ng.kithso.android.data.AlbumGroup
import ng.kithso.android.data.Artist
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.DeleteOutcome
import ng.kithso.android.data.DeleteSize
import ng.kithso.android.data.Deletion
import ng.kithso.android.data.DeviceRemover
import ng.kithso.android.data.LibraryDeleter
import ng.kithso.android.data.Playlist
import ng.kithso.android.data.PlaylistChoice
import ng.kithso.android.data.PlaylistEditor
import ng.kithso.android.data.Track
import ng.kithso.core.HerePreview
import ng.kithso.core.HereState
import ng.kithso.core.ProfileItemKind
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * The menus and dialogs for playlists, album groups, albums, artists and tracks, shown by
 * [PlaylistSheets] over everything: long-pressing a playlist, album group, album, artist or
 * track opens its menu ([playlistMenu], [albumGroupMenu], [albumMenu], [artistMenu],
 * [trackMenu]), and Add to playlist ([addToPlaylist]) opens the list of playlists to add to,
 * with New playlist… first; an album's Add to album group… likewise lists the album groups.
 * [editor] is null when the library can't be edited, and [deleter] when nothing can be
 * deleted from it (the made-up one): the menus then offer only playing. [remover] offers
 * Remove from this phone (or Sync to this phone) on albums and artists, where it applies.
 */
@Stable
class PlaylistActions(
    val editor: PlaylistEditor?,
    val deleter: LibraryDeleter? = null,
    val remover: DeviceRemover? = null,
) {
    internal var sheet by mutableStateOf<Sheet?>(null)
    internal var dialog by mutableStateOf<Dialog?>(null)

    fun playlistMenu(playlist: Playlist) {
        sheet = Sheet.PlaylistMenu(playlist)
    }

    fun albumGroupMenu(group: AlbumGroup) {
        sheet = Sheet.AlbumGroupMenu(group)
    }

    /** [groupUid]: the manual album group the album is listed from, which it can be taken out of. */
    fun albumMenu(album: Album, groupUid: String? = null) {
        sheet = Sheet.AlbumMenu(album, groupUid)
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
        data class AlbumGroupMenu(val group: AlbumGroup) : Sheet
        data class AlbumMenu(val album: Album, val groupUid: String?) : Sheet
        data class ArtistMenu(val artist: Artist) : Sheet
        data class TrackMenu(val track: Track, val removable: Boolean) : Sheet
        data class AddTo(val what: Addition) : Sheet
        data class AddToGroup(val albumUid: String) : Sheet
    }

    internal sealed interface Dialog {
        data class NewPlaylist(val what: Addition) : Dialog
        data class NewGroup(val albumUid: String) : Dialog
        // A playlist's, or if [group] an album group's.
        data class Rename(val uid: String, val name: String, val group: Boolean = false) : Dialog
        data class Delete(val uid: String, val name: String, val group: Boolean = false) : Dialog
        /** Deleting an album, artist or track from the library, which is asked first. */
        data class DeleteFromLibrary(val what: Deletion) : Dialog
        /** Removing an album or artist from this phone only, which is asked first. */
        data class RemoveHere(val kind: ProfileItemKind, val uid: String, val name: String) : Dialog
    }
}

/** One row of a menu sheet. */
private class SheetAction(val label: String, val icon: ImageVector, val onClick: () -> Unit)

/**
 * Shows [actions]' open menu or dialog, if any. [onPlay] plays a collection from its start,
 * or shuffled; [onDeleted] is called with a playlist or album group once it's been deleted,
 * to close its page.
 */
@Composable
fun PlaylistSheets(
    actions: PlaylistActions,
    onPlay: (CollectionRef, shuffle: Boolean) -> Unit,
    onDeleted: (CollectionRef) -> Unit,
) {
    val editor = actions.editor
    val deleter = actions.deleter
    val remover = actions.remover
    val scope = rememberCoroutineScope()
    val context = LocalContext.current.applicationContext
    val syncHereFailed = stringResource(R.string.sync_here_failed)
    // Remove from this phone or Sync to this phone for an album or artist, where it applies.
    @Composable
    fun hereAction(kind: ProfileItemKind, uid: String, name: String): SheetAction? {
        val state by produceState(HereState.Unavailable, kind, uid) {
            value = remover?.hereState(kind, uid) ?: HereState.Unavailable
        }
        return when (state) {
            HereState.Unavailable -> null
            HereState.Held -> SheetAction(stringResource(R.string.remove_from_this_phone), Icons.Remove) {
                actions.dialog = PlaylistActions.Dialog.RemoveHere(kind, uid, name)
            }
            HereState.Removed -> SheetAction(stringResource(R.string.sync_to_this_phone), Icons.Sync) {
                scope.launch {
                    if (remover?.restoreHere(kind, uid) == false) {
                        Toast.makeText(context, syncHereFailed, Toast.LENGTH_LONG).show()
                    }
                }
            }
        }
    }
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
        is PlaylistActions.Sheet.AlbumGroupMenu -> {
            val group = sheet.group
            val ref = CollectionRef(CollectionRef.Kind.AlbumGroup, group.uid)
            // Smart album groups can only be played.
            val editable by produceState(false, group.uid) { value = editor?.isEditableGroup(group.uid) == true }
            MenuSheet(
                actions,
                group.name,
                pluralStringResource(R.plurals.albums, group.albumCount, group.albumCount),
                buildList {
                    add(SheetAction(stringResource(R.string.play), Icons.Play) { onPlay(ref, false) })
                    add(SheetAction(stringResource(R.string.shuffle), Icons.Shuffle) { onPlay(ref, true) })
                    if (editable) {
                        add(SheetAction(stringResource(R.string.playlist_rename_menu), Icons.Edit) {
                            actions.dialog = PlaylistActions.Dialog.Rename(group.uid, group.name, group = true)
                        })
                        add(SheetAction(stringResource(R.string.album_group_delete_menu), Icons.Delete) {
                            actions.dialog = PlaylistActions.Dialog.Delete(group.uid, group.name, group = true)
                        })
                    }
                },
            )
        }
        is PlaylistActions.Sheet.AlbumMenu -> {
            val album = sheet.album
            val ref = CollectionRef(CollectionRef.Kind.Album, album.uid)
            val groupUid = sheet.groupUid
            val here = hereAction(ProfileItemKind.Album, album.uid, album.title)
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
                        add(SheetAction(stringResource(R.string.add_to_album_group_menu), Icons.LibraryAdd) {
                            actions.sheet = PlaylistActions.Sheet.AddToGroup(album.uid)
                        })
                        if (groupUid != null) {
                            add(SheetAction(stringResource(R.string.remove_from_album_group), Icons.Remove) {
                                editor.removeFromAlbumGroup(groupUid, album.uid)
                            })
                        }
                    }
                    if (here != null) add(here)
                    if (deleter != null) {
                        add(SheetAction(stringResource(R.string.delete_from_library_menu), Icons.Delete) {
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
            val here = hereAction(ProfileItemKind.Artist, artist.uid, artist.name)
            MenuSheet(
                actions,
                artist.name,
                artist.subtitle(),
                buildList {
                    add(SheetAction(stringResource(R.string.play), Icons.Play) { onPlay(ref, false) })
                    add(SheetAction(stringResource(R.string.shuffle), Icons.Shuffle) { onPlay(ref, true) })
                    if (here != null) add(here)
                    if (deleter != null) {
                        add(SheetAction(stringResource(R.string.delete_from_library_menu), Icons.Delete) {
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
                        add(SheetAction(stringResource(R.string.delete_from_library_menu), Icons.Delete) {
                            actions.dialog = PlaylistActions.Dialog.DeleteFromLibrary(
                                Deletion(Deletion.Kind.Track, track.uid, name),
                            )
                        })
                    }
                },
            )
        }
        is PlaylistActions.Sheet.AddTo -> if (editor != null) {
            AddToSheet(
                actions,
                title = stringResource(R.string.add_to_playlist),
                newLabel = stringResource(R.string.new_playlist_menu),
                icon = Icons.LibraryMusic,
                added = R.string.playlist_added,
                load = editor::playlistChoices,
                onNew = { actions.dialog = PlaylistActions.Dialog.NewPlaylist(sheet.what) },
                onPick = { editor.addToPlaylist(it.uid, sheet.what) },
            )
        }
        is PlaylistActions.Sheet.AddToGroup -> if (editor != null) {
            AddToSheet(
                actions,
                title = stringResource(R.string.add_to_album_group),
                newLabel = stringResource(R.string.new_album_group_menu),
                icon = Icons.Album,
                added = R.string.album_group_added,
                load = editor::albumGroupChoices,
                onNew = { actions.dialog = PlaylistActions.Dialog.NewGroup(sheet.albumUid) },
                onPick = { editor.addToAlbumGroup(it.uid, sheet.albumUid) },
            )
        }
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
        is PlaylistActions.Dialog.NewGroup -> NameDialog(
            title = stringResource(R.string.new_album_group),
            initial = "",
            confirm = stringResource(R.string.create),
            onConfirm = { name -> editor?.addToNewAlbumGroup(name, dialog.albumUid) },
            onDismiss = { actions.dialog = null },
        )
        is PlaylistActions.Dialog.Rename -> NameDialog(
            title = stringResource(if (dialog.group) R.string.album_group_rename_title else R.string.playlist_rename_title),
            initial = dialog.name,
            confirm = stringResource(R.string.rename),
            onConfirm = { name ->
                if (name != dialog.name) {
                    if (dialog.group) editor?.renameAlbumGroup(dialog.uid, name) else editor?.renamePlaylist(dialog.uid, name)
                }
            },
            onDismiss = { actions.dialog = null },
        )
        is PlaylistActions.Dialog.Delete -> AlertDialog(
            onDismissRequest = { actions.dialog = null },
            title = { Text(stringResource(R.string.playlist_delete_title, dialog.name)) },
            text = { Text(stringResource(if (dialog.group) R.string.album_group_delete_text else R.string.playlist_delete_text)) },
            confirmButton = {
                TextButton(onClick = {
                    actions.dialog = null
                    if (dialog.group) {
                        editor?.deleteAlbumGroup(dialog.uid)
                        onDeleted(CollectionRef(CollectionRef.Kind.AlbumGroup, dialog.uid))
                    } else {
                        editor?.deletePlaylist(dialog.uid)
                        onDeleted(CollectionRef(CollectionRef.Kind.Playlist, dialog.uid))
                    }
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
        is PlaylistActions.Dialog.RemoveHere -> if (remover != null) {
            RemoveHereDialog(remover, dialog, scope, onDismiss = { actions.dialog = null })
        }
    }
}

/**
 * Asks before removing an album or artist from this phone only: how many files go from here,
 * how many originals imported here stay until an archive has them, that it stays in the
 * library and on the other devices, and how to get it back. Remove waits for the count.
 * Afterwards a toast says if it failed, or if some files couldn't be removed.
 */
@Composable
private fun RemoveHereDialog(
    remover: DeviceRemover,
    what: PlaylistActions.Dialog.RemoveHere,
    // Outlives the dialog, which closes as the removal starts, for the toasts after it.
    scope: CoroutineScope,
    onDismiss: () -> Unit,
) {
    val context = LocalContext.current.applicationContext
    val failed = stringResource(R.string.remove_here_failed)
    val resources = context.resources
    val name = what.name.ifEmpty { stringResource(R.string.unknown_value) }
    val preview by produceState<HerePreview?>(null, what) {
        val found = remover.herePreview(what.kind, what.uid)
        if (found == null) {
            Toast.makeText(context, failed, Toast.LENGTH_LONG).show()
            onDismiss()
        } else {
            value = found
        }
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.remove_from_this_phone_title, name)) },
        text = {
            preview?.let {
                val parts = buildList {
                    if (it.files > 0) add(pluralStringResource(R.plurals.remove_here_files, it.files, it.files))
                    else if (it.kept == 0) add(stringResource(R.string.remove_here_no_files))
                    if (it.kept > 0) add(pluralStringResource(R.plurals.remove_here_kept, it.kept, it.kept))
                    add(stringResource(R.string.remove_from_this_phone_back))
                }
                Text(parts.joinToString(" "))
            }
        },
        confirmButton = {
            TextButton(
                enabled = preview != null,
                onClick = {
                    val kept = preview?.kept ?: 0
                    onDismiss()
                    scope.launch {
                        val result = remover.removeHere(what.kind, what.uid)
                        if (result == null) {
                            Toast.makeText(context, failed, Toast.LENGTH_LONG).show()
                        } else if (result.filesKept > kept) {
                            val n = result.filesKept - kept
                            Toast.makeText(
                                context,
                                resources.getQuantityString(R.plurals.remove_here_in_use, n, n),
                                Toast.LENGTH_LONG,
                            ).show()
                        }
                    }
                },
            ) {
                Text(stringResource(R.string.remove), color = MaterialTheme.colorScheme.error)
            }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) }
        },
    )
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
 * Add to playlist or Add to album group: [title] over New playlist… or New album group…
 * ([newLabel]; [onNew] asks for a name), then the manual playlists or album groups [load]
 * lists, those with nothing to play on this phone too. Picking one hands it to [onPick], and a
 * toast says where it went ([added], with its name).
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AddToSheet(
    actions: PlaylistActions,
    title: String,
    newLabel: String,
    icon: ImageVector,
    @StringRes added: Int,
    load: suspend () -> List<PlaylistChoice>?,
    onNew: () -> Unit,
    onPick: (PlaylistChoice) -> Unit,
) {
    val state = rememberModalBottomSheetState()
    val scope = rememberCoroutineScope()
    val context = LocalContext.current
    val current = actions.sheet
    val choices by produceState<List<PlaylistChoice>?>(null, current) { value = load() }
    val close: (() -> Unit) -> Unit = { then ->
        scope.launch { state.hide() }.invokeOnCompletion {
            if (actions.sheet == current) actions.sheet = null
        }
        then()
    }
    ModalBottomSheet(onDismissRequest = { if (actions.sheet == current) actions.sheet = null }, sheetState = state) {
        SheetTitle(title, null)
        LazyColumn(Modifier.navigationBarsPadding()) {
            item(key = "new") {
                ListItem(
                    headlineContent = { Text(newLabel) },
                    leadingContent = { Icon(Icons.Add, contentDescription = null) },
                    colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
                    modifier = Modifier.clickable { close(onNew) },
                )
            }
            items(choices.orEmpty(), key = { it.uid }) { choice ->
                val toast = stringResource(added, choice.name)
                ListItem(
                    headlineContent = { Text(choice.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    leadingContent = { Icon(icon, contentDescription = null) },
                    colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
                    modifier = Modifier.clickable {
                        close {
                            onPick(choice)
                            Toast.makeText(context, toast, Toast.LENGTH_SHORT).show()
                        }
                    },
                )
            }
        }
    }
}

/** Asks for a playlist's or album group's name, starting from [initial] (selected), for [onConfirm]. */
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
