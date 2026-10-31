package ng.kithso.android.ui

import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.Album
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.LibrarySource
import ng.kithso.android.data.Track
import ng.kithso.android.data.TrackList
import ng.kithso.android.ui.art.AlbumArt
import ng.kithso.android.ui.theme.ArtBackdrop
import ng.kithso.android.ui.theme.rememberArtColors
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch

/**
 * An album, artist, playlist or album group, over a background coloured from its art: its art and name, Play and Shuffle ([onPlayAll],
 * as the desktops have above a track list), then its tracks. Tapping a track plays the list
 * from there ([onPlay]), shuffled or not as shuffle is; long-pressing one opens its menu
 * ([PlaylistActions.trackMenu]). An artist's albums come before their tracks, and an album
 * group shows its albums; [onOpen] opens an album, and long-pressing one opens its menu
 * ([PlaylistActions.albumMenu]), which in a manual album group can take it out. A smart list in random order has a button
 * to reshuffle it. [onBack] slides the screen away; it is also dismissed by swiping right.
 *
 * A manual playlist has an Edit button, which shows each track with a button to remove it and
 * a handle to drag it elsewhere, until Done. Each change is made as it happens, and the list as
 * changed here ([edited]) is shown until the playlist has been read again. A manual album group
 * has one too, which lists its albums the same way ([editedAlbums]).
 */
@Composable
fun DetailScreen(
    ref: CollectionRef,
    source: LibrarySource,
    actions: PlaylistActions,
    bottomPadding: Dp,
    onPlay: (List<Track>, Int) -> Unit,
    onPlayAll: (List<Track>, shuffle: Boolean) -> Unit,
    onOpen: (CollectionRef) -> Unit,
    onBack: () -> Unit,
) {
    val editor = actions.editor
    // Bumped by a reshuffle, to list it again in its new order.
    var shuffles by remember { mutableIntStateOf(0) }
    val scope = rememberCoroutineScope()
    // An edit reads the list again, even when the library looks no different (after a move).
    val edits by (editor?.edits ?: remember { MutableStateFlow(0) }).collectAsState()
    var edited by remember { mutableStateOf<List<Track>?>(null) }
    var editedAlbums by remember { mutableStateOf<List<Album>?>(null) }
    val loaded by produceState<TrackList?>(null, ref, edits, shuffles) {
        val list = source.trackList(ref)
        // An edit took the playlist's last track on this phone, or deleted it: it's gone.
        if (list == null && value != null) onBack()
        value = list
        edited = null
        editedAlbums = null
    }
    val editable by produceState(false, ref, edits) {
        value = ref.kind == CollectionRef.Kind.Playlist && editor?.isEditable(ref.uid) == true
    }
    // A manual album group's albums can be taken out of it, from their menus, and reordered.
    val editableGroup by produceState(false, ref, edits) {
        value = ref.kind == CollectionRef.Kind.AlbumGroup && editor?.isEditableGroup(ref.uid) == true
    }
    val albumMenu: (Album) -> Unit = { actions.albumMenu(it, groupUid = ref.uid.takeIf { editableGroup }) }
    var editing by rememberSaveable { mutableStateOf(false) }

    // The art is the library's: it's known before the list is read, so the background starts
    // in its colours. A playlist's or album group's picture is looked up after the library is
    // read, so it may come later.
    val library by source.library.collectAsState()
    val picture = remember(library, ref) { library?.artOf(ref) }
    val list = loaded?.let { if (picture != null) it.copy(art = picture) else it }
    val tracks = edited ?: list?.tracks.orEmpty()
    val albums = editedAlbums ?: list?.albums.orEmpty()
    val isEditing = editing && (editable || editableGroup)

    val listState = rememberLazyListState()
    val reorder = remember(listState) { ReorderState(listState) }
    // Where the dragged track or album was before this drag.
    var dragFrom by remember { mutableIntStateOf(-1) }
    reorder.isMovable = { it is String && (it.startsWith(ENTRY_KEY) || it.startsWith(ALBUM_KEY)) }
    reorder.onMove = { from, to ->
        if (from is String && from.startsWith(ALBUM_KEY)) {
            val i = albums.indexOfFirst { ALBUM_KEY + it.uid == from }
            val j = albums.indexOfFirst { ALBUM_KEY + it.uid == to }
            if (i >= 0 && j >= 0) {
                if (dragFrom < 0) dragFrom = i
                editedAlbums = albums.toMutableList().apply { add(j, removeAt(i)) }
            }
        } else {
            val i = tracks.indexOfFirst { entryKey(it) == from }
            val j = tracks.indexOfFirst { entryKey(it) == to }
            if (i >= 0 && j >= 0) {
                if (dragFrom < 0) dragFrom = i
                edited = tracks.toMutableList().apply { add(j, removeAt(i)) }
            }
        }
    }
    reorder.onDrop = { key ->
        if (key is String && key.startsWith(ALBUM_KEY)) {
            val i = albums.indexOfFirst { ALBUM_KEY + it.uid == key }
            val album = albums.getOrNull(i)?.uid
            if (album != null && dragFrom >= 0 && i != dragFrom) editor?.moveInAlbumGroup(ref.uid, album, albums.getOrNull(i - 1)?.uid)
        } else {
            val i = tracks.indexOfFirst { entryKey(it) == key }
            val entry = tracks.getOrNull(i)?.entryUid
            if (entry != null && dragFrom >= 0 && i != dragFrom) editor?.moveEntry(entry, tracks.getOrNull(i - 1)?.entryUid)
        }
        dragFrom = -1
    }
    // Moves a track by one place, for accessibility services.
    val moveBy: (Int, Int) -> Unit = { i, by ->
        val entry = tracks.getOrNull(i)?.entryUid
        if (entry != null && i + by in tracks.indices) {
            val moved = tracks.toMutableList().apply { add(i + by, removeAt(i)) }
            edited = moved
            editor?.moveEntry(entry, moved.getOrNull(i + by - 1)?.entryUid)
        }
    }
    // Likewise an album in a group.
    val moveAlbumBy: (Int, Int) -> Unit = { i, by ->
        val album = albums.getOrNull(i)?.uid
        if (album != null && i + by in albums.indices) {
            val moved = albums.toMutableList().apply { add(i + by, removeAt(i)) }
            editedAlbums = moved
            editor?.moveInAlbumGroup(ref.uid, album, moved.getOrNull(i + by - 1)?.uid)
        }
    }

    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        BoxWithConstraints(Modifier.fillMaxSize()) {
            val columns = albumColumns(maxWidth)
            val group = ref.kind == CollectionRef.Kind.AlbumGroup
            ArtBackdrop(rememberArtColors(picture ?: list?.art), Modifier.matchParentSize())
            val density = LocalDensity.current
            val statusBarPx = WindowInsets.statusBars.getTop(density)
            val sides = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
            // An artist's page jumps between its headings, and through its tracks by title.
            val scrollEntries = if (ref.kind == CollectionRef.Kind.Artist && list != null) {
                val titles = listOf(
                    stringResource(R.string.section_albums),
                    stringResource(R.string.section_singles),
                    stringResource(R.string.tracks_title),
                )
                val other = stringResource(R.string.fast_scroll_other)
                val hasActions = !isEditing && (tracks.isNotEmpty() || list.canReshuffle)
                remember(list, albums, tracks, hasActions, titles, other) {
                    artistScrollEntries(albums.size, list.singles.size, tracks, hasActions, titles, other)
                }
            } else {
                emptyList()
            }
            LazyColumn(
                state = listState,
                contentPadding = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
                    .add(WindowInsets(bottom = bottomPadding))
                    .asPaddingValues(),
                modifier = Modifier.fillMaxSize(),
            ) {
                item(key = "header", contentType = "header") {
                    DetailHeader(
                        ref.kind,
                        list,
                        onBack,
                        editing = if (editable || editableGroup) isEditing else null,
                        onEdit = { editing = it },
                        // An album's menu, as its long press elsewhere opens, for Delete from library and the rest.
                        onMenu = if (ref.kind == CollectionRef.Kind.Album && list != null) ({
                            actions.albumMenu(Album(ref.uid, list.title, list.artist, list.year, list.art))
                        }) else null,
                    )
                }
                if (list != null && !isEditing && (tracks.isNotEmpty() || list.canReshuffle)) item(key = "actions", contentType = "actions") {
                    DetailActions(
                        onPlay = if (tracks.isNotEmpty()) ({ shuffle -> onPlayAll(tracks, shuffle) }) else null,
                        onReshuffle = if (list.canReshuffle) ({
                            scope.launch {
                                source.reshuffle(ref)
                                shuffles++
                            }
                        }) else null,
                    )
                }
                if (group && isEditing) {
                    itemsIndexed(albums, key = { _, album -> ALBUM_KEY + album.uid }, contentType = { _, _ -> "album" }) { i, album ->
                        val key = ALBUM_KEY + album.uid
                        val moveUp = stringResource(R.string.move_up)
                        val moveDown = stringResource(R.string.move_down)
                        EditAlbumRow(
                            album,
                            onRemove = {
                                editedAlbums = albums.filterIndexed { j, _ -> j != i }
                                editor?.removeFromAlbumGroup(ref.uid, album.uid)
                            },
                            handle = with(reorder) {
                                Modifier.dragHandle(key).semantics {
                                    customActions = buildList {
                                        if (i > 0) add(CustomAccessibilityAction(moveUp) { moveAlbumBy(i, -1); true })
                                        if (i < albums.lastIndex) add(CustomAccessibilityAction(moveDown) { moveAlbumBy(i, 1); true })
                                    }
                                }
                            },
                            lifted = key == reorder.dragged,
                            modifier = if (key == reorder.dragged) with(reorder) { Modifier.dragged(key) } else Modifier.animateItem(),
                        )
                    }
                    return@LazyColumn
                }
                if (group) {
                    items(albums.chunked(columns), key = { "albums:${it.first().uid}" }, contentType = { "albums" }) { row ->
                        AlbumGridRow(row, columns, onOpen, albumMenu)
                    }
                    return@LazyColumn
                }
                // An artist's albums, then their singles, which play among themselves, then all
                // their tracks; the headings only when there's more than the tracks.
                if (ref.kind == CollectionRef.Kind.Artist) {
                    val singles = list?.singles.orEmpty()
                    if (albums.isNotEmpty()) item(key = "albums", contentType = "albums") {
                        Column {
                            BrowseHeading(stringResource(R.string.section_albums))
                            AlbumStrip(albums, list?.title.orEmpty(), onOpen, albumMenu)
                        }
                    }
                    if (singles.isNotEmpty()) {
                        item(key = "singles", contentType = "heading") { BrowseHeading(stringResource(R.string.section_singles)) }
                        itemsIndexed(singles, key = { _, track -> SINGLE_KEY + track.uid }, contentType = { _, _ -> "track" }) { i, track ->
                            TrackRow(
                                track,
                                showAlbum = true,
                                onLongClick = { actions.trackMenu(track, removable = false, from = ref) },
                                onClick = { onPlay(singles, i) },
                            )
                        }
                    }
                    if ((albums.isNotEmpty() || singles.isNotEmpty()) && tracks.isNotEmpty()) {
                        item(key = "tracks", contentType = "heading") { BrowseHeading(stringResource(R.string.tracks_title)) }
                    }
                }
                // A playlist can hold a track twice, so the uid alone isn't a unique key. Its
                // entries are, and keep a dragged track's key as it moves.
                itemsIndexed(tracks, key = { i, track -> entryKey(track) ?: "$i:${track.uid}" }, contentType = { _, _ -> "track" }) { i, track ->
                    val key = entryKey(track)
                    if (isEditing && key != null) {
                        val moveUp = stringResource(R.string.move_up)
                        val moveDown = stringResource(R.string.move_down)
                        EditTrackRow(
                            track,
                            onRemove = {
                                edited = tracks.filterIndexed { j, _ -> j != i }
                                track.entryUid?.let { editor?.removeEntry(it) }
                            },
                            handle = with(reorder) {
                                Modifier.dragHandle(key).semantics {
                                    customActions = buildList {
                                        if (i > 0) add(CustomAccessibilityAction(moveUp) { moveBy(i, -1); true })
                                        if (i < tracks.lastIndex) add(CustomAccessibilityAction(moveDown) { moveBy(i, 1); true })
                                    }
                                }
                            },
                            lifted = key == reorder.dragged,
                            modifier = if (key == reorder.dragged) with(reorder) { Modifier.dragged(key) } else Modifier.animateItem(),
                        )
                    } else {
                        TrackRow(
                            track,
                            showAlbum = ref.kind != CollectionRef.Kind.Album,
                            onLongClick = { actions.trackMenu(track, removable = editable, from = ref) },
                            onClick = { onPlay(tracks, i) },
                        )
                    }
                }
            }
            FastScroller(
                listState,
                scrollEntries,
                scrollOffset = -statusBarPx,
                modifier = Modifier
                    .matchParentSize()
                    .windowInsetsPadding(sides)
                    .padding(top = with(density) { statusBarPx.toDp() } + 8.dp, bottom = bottomPadding + 8.dp, end = 2.dp),
            )
        }
    }
}

/**
 * The fast scroll strip's entries for an artist's page, laid out as [DetailScreen] lays it out:
 * the header, the play buttons if [actions], then the [albumCount] albums' strip, the
 * [singleCount] singles and the [tracks] (in order of title), with headings ([titles]: albums,
 * singles, tracks) only when there's more than the tracks. Letters for a long enough list of
 * tracks; [other] is the label of tracks under [OTHER_LETTER].
 */
private fun artistScrollEntries(
    albumCount: Int,
    singleCount: Int,
    tracks: List<Track>,
    actions: Boolean,
    titles: List<String>,
    other: String,
): List<FastScrollEntry> = buildList {
    val (albumsTitle, singlesTitle, tracksTitle) = titles
    var i = if (actions) 2 else 1
    if (albumCount > 0) add(FastScrollEntry(albumsTitle, i++, heading = true))
    if (singleCount > 0) {
        add(FastScrollEntry(singlesTitle, i, heading = true))
        i += 1 + singleCount
    }
    val headed = (albumCount > 0 || singleCount > 0) && tracks.isNotEmpty()
    if (headed) add(FastScrollEntry(tracksTitle, i++, heading = true))
    val start = i
    if (tracks.size >= TRACK_LETTERS_FROM) {
        letterEntries(this, tracks.map { it.title }, other, tracksTitle.takeIf { headed }) { start + it }
    }
}

/** How many tracks an artist's page has before the fast scroll strip has their letters. */
private const val TRACK_LETTERS_FROM = 12

/** Play and Shuffle ([onPlay], with whether to shuffle), and Reshuffle, if given. */
@Composable
private fun DetailActions(onPlay: ((Boolean) -> Unit)?, onReshuffle: (() -> Unit)?) {
    FlowRow(
        Modifier.padding(start = 16.dp, end = 16.dp, bottom = 12.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (onPlay != null) {
            Button(onClick = { onPlay(false) }) {
                Icon(Icons.Play, contentDescription = null, modifier = Modifier.size(ButtonDefaults.IconSize))
                Text(stringResource(R.string.play), modifier = Modifier.padding(start = ButtonDefaults.IconSpacing))
            }
            FilledTonalButton(onClick = { onPlay(true) }) {
                Icon(Icons.Shuffle, contentDescription = null, modifier = Modifier.size(ButtonDefaults.IconSize))
                Text(stringResource(R.string.shuffle), modifier = Modifier.padding(start = ButtonDefaults.IconSpacing))
            }
        }
        if (onReshuffle != null) OutlinedButton(onClick = onReshuffle) {
            Icon(Icons.Shuffle, contentDescription = null, modifier = Modifier.size(ButtonDefaults.IconSize))
            Text(stringResource(R.string.reshuffle), modifier = Modifier.padding(start = ButtonDefaults.IconSpacing))
        }
    }
}

/**
 * The back button, then the art, title and subtitle beside each other. In a short window
 * (landscape, mostly) the back button goes beside them too, and the art is smaller, to leave
 * room for the tracks. A playlist that can be edited has Edit, or Done while [editing], at
 * the other end from the back button, as has a manual album group; [editing] is null for anything else.
 * An album has a menu button there instead ([onMenu]).
 */
@Composable
private fun DetailHeader(
    kind: CollectionRef.Kind,
    list: TrackList?,
    onBack: () -> Unit,
    editing: Boolean?,
    onEdit: (Boolean) -> Unit,
    onMenu: (() -> Unit)? = null,
) {
    val short = isShortWindow()
    val back = @Composable {
        IconButton(onClick = onBack, modifier = Modifier.padding(start = 4.dp, top = 4.dp)) {
            Icon(Icons.ArrowBack, contentDescription = stringResource(R.string.back))
        }
    }
    val edit = @Composable {
        if (editing != null) {
            TextButton(onClick = { onEdit(!editing) }, modifier = Modifier.padding(end = 8.dp, top = 4.dp)) {
                Text(stringResource(if (editing) R.string.done else R.string.edit_playlist))
            }
        } else if (onMenu != null) {
            IconButton(onClick = onMenu, modifier = Modifier.padding(end = 4.dp, top = 4.dp)) {
                Icon(Icons.MoreVert, contentDescription = stringResource(R.string.album_menu))
            }
        }
    }
    if (short) {
        Row(Modifier.fillMaxWidth().windowInsetsPadding(WindowInsets.statusBars)) {
            back()
            if (list != null) DetailTitle(kind, list, 96.dp, Modifier.weight(1f).padding(start = 4.dp, top = 12.dp))
            edit()
        }
    } else {
        Column(Modifier.fillMaxWidth().windowInsetsPadding(WindowInsets.statusBars)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                back()
                Spacer(Modifier.weight(1f))
                edit()
            }
            if (list != null) DetailTitle(kind, list, 128.dp, Modifier.padding(start = 16.dp))
        }
    }
}

@Composable
private fun DetailTitle(kind: CollectionRef.Kind, list: TrackList, artSize: Dp, modifier: Modifier) {
    Row(modifier.padding(end = 16.dp, bottom = 16.dp), verticalAlignment = Alignment.CenterVertically) {
        AlbumArt(
            list.art,
            contentDescription = when (kind) {
                CollectionRef.Kind.Album -> stringResource(R.string.cover_of, list.title)
                else -> null
            },
            shape = if (kind == CollectionRef.Kind.Artist) CircleShape else RoundedCornerShape(16.dp),
            modifier = Modifier.size(artSize),
        )
        Column(Modifier.padding(start = 16.dp).widthIn(min = 0.dp)) {
            Text(
                list.title,
                style = MaterialTheme.typography.headlineSmall,
                maxLines = 3,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.semantics { heading() },
            )
            Text(
                when (kind) {
                    CollectionRef.Kind.Album ->
                        if (list.year > 0) stringResource(R.string.album_byline_year, list.artist, list.year) else list.artist
                    CollectionRef.Kind.Artist ->
                        if (list.albumCount > 0) pluralStringResource(R.plurals.albums, list.albumCount, list.albumCount)
                        else pluralStringResource(R.plurals.tracks, list.tracks.size, list.tracks.size)
                    CollectionRef.Kind.Playlist -> pluralStringResource(R.plurals.tracks, list.tracks.size, list.tracks.size)
                    CollectionRef.Kind.AlbumGroup -> pluralStringResource(R.plurals.albums, list.albumCount, list.albumCount)
                },
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun TrackRow(track: Track, showAlbum: Boolean, onLongClick: () -> Unit, onClick: () -> Unit) {
    ListItem(
        leadingContent = if (showAlbum) {
            { AlbumArt(track.art, contentDescription = null, shape = RoundedCornerShape(6.dp), modifier = Modifier.size(40.dp)) }
        } else {
            {
                Box(Modifier.size(40.dp), contentAlignment = Alignment.Center) {
                    Text(
                        track.trackNo.toString(),
                        style = MaterialTheme.typography.bodyLarge,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        },
        headlineContent = { Text(track.title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = if (showAlbum) {
            { Text(trackByline(track.artist, track.album), maxLines = 1, overflow = TextOverflow.Ellipsis) }
        } else null,
        trailingContent = { Text(formatTime(track.durationMs)) },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.combinedClickable(onLongClick = onLongClick, onClick = onClick),
    )
}

/**
 * A playlist's track while editing: a button to remove it, and [handle] to drag it by. It's
 * clear, for the background, except while [lifted] by a drag, so the rows it passes don't
 * show through.
 */
@Composable
private fun EditTrackRow(track: Track, onRemove: () -> Unit, handle: Modifier, lifted: Boolean, modifier: Modifier) {
    ListItem(
        leadingContent = {
            IconButton(onClick = onRemove) {
                Icon(
                    Icons.RemoveCircle,
                    contentDescription = stringResource(R.string.remove_from_playlist),
                    tint = MaterialTheme.colorScheme.error,
                )
            }
        },
        headlineContent = { Text(track.title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = {
            Text(trackByline(track.artist, track.album), maxLines = 1, overflow = TextOverflow.Ellipsis)
        },
        trailingContent = {
            Box(handle.size(48.dp), contentAlignment = Alignment.Center) {
                Icon(Icons.DragHandle, contentDescription = stringResource(R.string.playlist_reorder))
            }
        },
        colors = ListItemDefaults.colors(
            containerColor = if (lifted) MaterialTheme.colorScheme.surfaceContainerHigh else Color.Transparent,
        ),
        modifier = modifier,
    )
}

/** An album group's album while editing, as [EditTrackRow] is a playlist's track. */
@Composable
private fun EditAlbumRow(album: Album, onRemove: () -> Unit, handle: Modifier, lifted: Boolean, modifier: Modifier) {
    ListItem(
        leadingContent = {
            Row(verticalAlignment = Alignment.CenterVertically) {
                IconButton(onClick = onRemove) {
                    Icon(
                        Icons.RemoveCircle,
                        contentDescription = stringResource(R.string.remove_from_album_group),
                        tint = MaterialTheme.colorScheme.error,
                    )
                }
                AlbumArt(album.art, contentDescription = null, shape = RoundedCornerShape(6.dp), modifier = Modifier.size(40.dp))
            }
        },
        headlineContent = { Text(album.title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = { Text(album.artist, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        trailingContent = {
            Box(handle.size(48.dp), contentAlignment = Alignment.Center) {
                Icon(Icons.DragHandle, contentDescription = stringResource(R.string.playlist_reorder))
            }
        },
        colors = ListItemDefaults.colors(
            containerColor = if (lifted) MaterialTheme.colorScheme.surfaceContainerHigh else Color.Transparent,
        ),
        modifier = modifier,
    )
}

private const val ENTRY_KEY = "entry:"
private const val ALBUM_KEY = "album:"
private const val SINGLE_KEY = "single:"

// A playlist track's key in the list: its entry, which is unique and stays the same as it moves.
private fun entryKey(track: Track): String? = track.entryUid?.let { ENTRY_KEY + it }
