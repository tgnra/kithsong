package ng.kithso.android.ui

import androidx.annotation.StringRes
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.indication
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.PressInteraction
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyItemScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.ripple
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.AwaitPointerEventScope
import androidx.compose.ui.input.pointer.PointerInputChange
import androidx.compose.ui.input.pointer.changedToUp
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.LayoutCoordinates
import androidx.compose.ui.layout.boundsInWindow
import androidx.compose.ui.layout.layout
import androidx.compose.ui.layout.onPlaced
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.Album
import ng.kithso.android.data.AlbumGroup
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.Artist
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Library
import ng.kithso.android.data.Playlist
import ng.kithso.android.data.Recent
import ng.kithso.android.ui.art.AlbumArt
import ng.kithso.android.ui.art.PrefetchArt
import ng.kithso.android.ui.theme.ArtBackdrop
import ng.kithso.android.ui.theme.rememberOnScreenArtColors
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.launch
import kotlin.math.floor
import kotlin.math.roundToInt

enum class Section(@StringRes val title: Int) {
    Recents(R.string.section_recents),
    Playlists(R.string.section_playlists),
    AlbumGroups(R.string.section_album_groups),
    Albums(R.string.section_albums),
    Artists(R.string.section_artists),
}

/** One item of the main list. Albums come [MainItem.AlbumRow] at a time, a grid row each. */
@Immutable
private sealed interface MainItem {
    val key: Any
    val contentType: Any
    val art: List<ArtRef> get() = emptyList()

    data object Header : MainItem {
        override val key get() = "header"
        override val contentType get() = "header"
    }

    data object Banner : MainItem {
        override val key get() = "banner"
        override val contentType get() = "banner"
    }

    data object Empty : MainItem {
        override val key get() = "empty"
        override val contentType get() = "empty"
    }

    data class Heading(val section: Section) : MainItem {
        override val key get() = "heading:$section"
        override val contentType get() = "heading"
    }

    data class RecentItem(val recent: Recent) : MainItem {
        override val key get() = "recent:${recent.ref.kind}:${recent.ref.uid}"
        override val contentType get() = "recent"
        override val art get() = listOf(recent.art)
    }

    data class PlaylistItem(val playlist: Playlist) : MainItem {
        override val key get() = "playlist:${playlist.uid}"
        override val contentType get() = "playlist"
        override val art get() = listOf(playlist.art)
    }

    data class AlbumGroupItem(val group: AlbumGroup) : MainItem {
        override val key get() = "group:${group.uid}"
        override val contentType get() = "playlist"
        override val art get() = listOf(group.art)
    }

    data class AlbumRow(val albums: List<Album>) : MainItem {
        override val key get() = "albums:${albums.first().uid}"
        override val contentType get() = "albums"
        override val art get() = albums.map { it.art }
    }

    data class ArtistItem(val artist: Artist) : MainItem {
        override val key get() = "artist:${artist.uid}"
        override val contentType get() = "artist"
        override val art get() = listOf(artist.art)
    }
}

// Null while the library is being read: just the header, rather than a flash of the empty state.
private fun mainItems(library: Library?, columns: Int, banner: Boolean): List<MainItem> = buildList {
    add(MainItem.Header)
    if (banner) add(MainItem.Banner)
    if (library == null) return@buildList
    if (library.isEmpty) {
        add(MainItem.Empty)
        return@buildList
    }
    if (library.recents.isNotEmpty()) {
        add(MainItem.Heading(Section.Recents))
        library.recents.mapTo(this, MainItem::RecentItem)
    }
    if (library.playlists.isNotEmpty()) {
        add(MainItem.Heading(Section.Playlists))
        library.playlists.mapTo(this, MainItem::PlaylistItem)
    }
    if (library.albumGroups.isNotEmpty()) {
        add(MainItem.Heading(Section.AlbumGroups))
        library.albumGroups.mapTo(this, MainItem::AlbumGroupItem)
    }
    if (library.albums.isNotEmpty()) {
        add(MainItem.Heading(Section.Albums))
        library.albums.chunked(columns).mapTo(this, MainItem::AlbumRow)
    }
    if (library.artists.isNotEmpty()) {
        add(MainItem.Heading(Section.Artists))
        library.artists.mapTo(this, MainItem::ArtistItem)
    }
}

/** A section heading's place in the list, for jumping there. */
@Immutable
private data class SectionStart(val section: Section, val index: Int)

private val Gutter = BrowseGutter
private val ListArtSize = 56.dp

/**
 * The main screen: one lazy list of a header and the library's sections, over a background
 * coloured from the art on screen. [bottomPadding] keeps the last items clear of the mini
 * player and navigation bar. [banner], if any, goes under the header: how an import is going.
 * The header's import button and the empty library's open [importPickers], unless an import
 * is [importRunning].
 */
@Composable
fun MainScreen(
    library: Library?,
    bottomPadding: Dp,
    onOpen: (CollectionRef) -> Unit,
    onSearch: () -> Unit,
    onSync: () -> Unit,
    onSettings: () -> Unit,
    importPickers: ImportPickers,
    importRunning: Boolean,
    banner: (@Composable (Modifier) -> Unit)?,
    modifier: Modifier = Modifier,
    // In a group, the empty library offers this phone's sync settings rather than pairing.
    inGroup: Boolean = false,
    onChooseWhatToSync: () -> Unit = {},
    // Long-pressing a recent, a playlist, an album group, an album or an artist opens its menu.
    onRecentMenu: (Recent) -> Unit = {},
    onPlaylistMenu: (Playlist) -> Unit = {},
    onAlbumMenu: (Album) -> Unit = {},
    onArtistMenu: (Artist) -> Unit = {},
    onAlbumGroupMenu: (AlbumGroup) -> Unit = {},
) {
    BoxWithConstraints(modifier.fillMaxSize()) {
        val density = LocalDensity.current
        // The list is inset from a camera cutout or navigation bar at the side, in landscape.
        val sides = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
        val layoutDirection = LocalLayoutDirection.current
        val width = maxWidth - with(density) { (sides.getLeft(density, layoutDirection) + sides.getRight(density, layoutDirection)).toDp() }
        val columns = albumColumns(width)
        val cellPx = with(density) { ((width - Gutter * 2 - TileSpacing * (columns - 1)) / columns).roundToPx() }
        val statusBarPx = WindowInsets.statusBars.getTop(density)

        val hasBanner = banner != null
        val items = remember(library, columns, hasBanner) { mainItems(library, columns, hasBanner) }
        val sections = remember(items) {
            items.mapIndexedNotNull { i, item -> (item as? MainItem.Heading)?.let { SectionStart(it.section, i) } }
        }
        val listState = rememberLazyListState()
        // The section at the top of the screen; the header counts as part of the first.
        val current by remember(sections) {
            derivedStateOf {
                (sections.lastOrNull { it.index <= listState.firstVisibleItemIndex } ?: sections.firstOrNull())?.section
            }
        }
        val scope = rememberCoroutineScope()
        // Lands the heading just below the status bar.
        val jumpTo: (Int) -> Unit = remember(listState, statusBarPx) {
            { index -> scope.launch { listState.animateScrollToItem(index, -statusBarPx) } }
        }
        // Holding a heading opens this over the list.
        val picker = remember(density) { SectionPicker(with(density) { PickerItemHeight.toPx() }) }

        PrefetchArt(listState, cellPx) { index -> items.getOrNull(index)?.art.orEmpty() }
        ArtBackdrop(
            rememberOnScreenArtColors(listState) { index -> items.getOrNull(index)?.art.orEmpty() },
            Modifier.matchParentSize(),
        )

        LazyColumn(
            state = listState,
            // The top is left to the header, so content scrolls under the status bar.
            contentPadding = sides
                .add(WindowInsets(bottom = bottomPadding))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            items(items, key = { it.key }, contentType = { it.contentType }) { item ->
                when (item) {
                    MainItem.Header -> Header(onSearch, onSync, onSettings, importPickers, importRunning)
                    MainItem.Banner -> banner?.invoke(Modifier.padding(horizontal = Gutter).padding(bottom = 8.dp))
                    MainItem.Empty -> EmptyLibrary(importPickers, importRunning, inGroup, onChooseWhatToSync)
                    is MainItem.Heading -> SectionHeading(item.section, sections, { current }, picker, jumpTo)
                    is MainItem.RecentItem -> LibraryRow(
                        item.recent.title,
                        item.recent.subtitle(),
                        item.recent.art,
                        RoundedCornerShape(8.dp),
                        onLongClick = { onRecentMenu(item.recent) },
                    ) { onOpen(item.recent.ref) }
                    is MainItem.PlaylistItem -> LibraryRow(
                        item.playlist.name,
                        pluralStringResource(R.plurals.tracks, item.playlist.trackCount, item.playlist.trackCount),
                        item.playlist.art,
                        RoundedCornerShape(8.dp),
                        onLongClick = { onPlaylistMenu(item.playlist) },
                    ) { onOpen(CollectionRef(CollectionRef.Kind.Playlist, item.playlist.uid)) }
                    is MainItem.AlbumGroupItem -> LibraryRow(
                        item.group.name,
                        pluralStringResource(R.plurals.albums, item.group.albumCount, item.group.albumCount),
                        item.group.art,
                        RoundedCornerShape(8.dp),
                        onLongClick = { onAlbumGroupMenu(item.group) },
                    ) { onOpen(CollectionRef(CollectionRef.Kind.AlbumGroup, item.group.uid)) }
                    is MainItem.AlbumRow -> AlbumGridRow(item.albums, columns, onOpen, onAlbumMenu)
                    is MainItem.ArtistItem -> LibraryRow(
                        item.artist.name,
                        item.artist.subtitle(),
                        item.artist.art,
                        CircleShape,
                        onLongClick = { onArtistMenu(item.artist) },
                    ) { onOpen(CollectionRef(CollectionRef.Kind.Artist, item.artist.uid)) }
                }
            }
        }
        SectionPickerOverlay(
            picker,
            sections,
            topInset = with(density) { statusBarPx.toDp() } + 8.dp,
            bottomInset = bottomPadding + 8.dp,
            modifier = Modifier.matchParentSize(),
        )
    }
}

/** The app title, with import, search, sync and settings. It scrolls away with the list. */
@Composable
private fun Header(
    onSearch: () -> Unit,
    onSync: () -> Unit,
    onSettings: () -> Unit,
    importPickers: ImportPickers,
    importRunning: Boolean,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .windowInsetsPadding(WindowInsets.statusBars)
            .padding(start = Gutter, end = 4.dp, top = 8.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            stringResource(R.string.app_name),
            style = MaterialTheme.typography.headlineMedium,
            modifier = Modifier.weight(1f).semantics { heading() },
        )
        ImportButton(importRunning, importPickers)
        IconButton(onClick = onSearch) { Icon(Icons.Search, contentDescription = stringResource(R.string.search)) }
        IconButton(onClick = onSync) { Icon(Icons.Sync, contentDescription = stringResource(R.string.sync)) }
        IconButton(onClick = onSettings) { Icon(Icons.Settings, contentDescription = stringResource(R.string.settings)) }
    }
}

/**
 * A section's heading, a tap target across the whole row. Tapping it opens a list of every
 * section, and picking one scrolls there; [current] is the section at the top of the screen,
 * marked in the list. Holding it opens [picker] under the finger instead: dragging up and down
 * moves through the sections, and letting go jumps to the one under the finger, all with one
 * thumb. TalkBack gets the tap, and a jump to each other section as a custom action.
 */
@Composable
private fun SectionHeading(
    section: Section,
    sections: List<SectionStart>,
    current: () -> Section?,
    picker: SectionPicker,
    onJump: (Int) -> Unit,
) {
    var open by remember { mutableStateOf(false) }
    val interactions = remember { MutableInteractionSource() }
    val haptics = LocalHapticFeedback.current
    val placed = remember { Placed() }
    val jumpLabel = stringResource(R.string.jump_to_section)
    val jumpActions = sections.filter { it.section != section }.map { start ->
        val label = stringResource(R.string.jump_to_named_section, stringResource(start.section.title))
        CustomAccessibilityAction(label) { onJump(start.index); true }
    }
    Box(Modifier.padding(top = 12.dp)) {
        Row(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 52.dp)
                .onPlaced { placed.coordinates = it }
                .indication(interactions, ripple())
                .pointerInput(section, sections, picker) {
                    awaitEachGesture {
                        val down = awaitFirstDown()
                        val press = PressInteraction.Press(down.position)
                        interactions.tryEmit(press)
                        // True for a tap, false for a scroll (or anything else taking the pointer), null for a hold.
                        val tapped = withTimeoutOrNull(viewConfiguration.longPressTimeoutMillis) { awaitTap(down) }
                        val coordinates = placed.coordinates
                        val start = sections.indexOfFirst { it.section == section }
                        if (tapped != null || coordinates == null || start < 0) {
                            if (tapped == true) {
                                interactions.tryEmit(PressInteraction.Release(press))
                                open = true
                            } else {
                                interactions.tryEmit(PressInteraction.Cancel(press))
                            }
                            return@awaitEachGesture
                        }
                        haptics.performHapticFeedback(HapticFeedbackType.LongPress)
                        picker.open(coordinates.localToWindow(down.position), start, sections.size)
                        // Follow the finger until it lifts (jump) or the gesture is taken away (don't).
                        var lifted = false
                        try {
                            while (true) {
                                val change = awaitPointerEvent().changes.firstOrNull { it.id == down.id } ?: break
                                change.consume()
                                if (!change.pressed) {
                                    lifted = true
                                    break
                                }
                                picker.fingerY = coordinates.localToWindow(change.position).y
                            }
                        } finally {
                            val chosen = picker.selected
                            picker.close()
                            interactions.tryEmit(PressInteraction.Release(press))
                            if (lifted) onJump(sections[chosen].index)
                        }
                    }
                }
                .semantics(mergeDescendants = true) {
                    heading()
                    onClick(jumpLabel) { open = true; true }
                    customActions = jumpActions
                }
                .padding(horizontal = Gutter, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(stringResource(section.title), style = MaterialTheme.typography.titleLarge)
            Icon(Icons.ExpandMore, contentDescription = null, modifier = Modifier.padding(start = 4.dp))
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }, offset = DpOffset(Gutter - 8.dp, 0.dp)) {
            val here = current()
            for (start in sections) {
                DropdownMenuItem(
                    text = {
                        Text(
                            stringResource(start.section.title),
                            color = if (start.section == here) MaterialTheme.colorScheme.primary else Color.Unspecified,
                            fontWeight = if (start.section == here) FontWeight.Bold else null,
                        )
                    },
                    onClick = {
                        open = false
                        onJump(start.index)
                    },
                )
            }
        }
    }
}

/** Waits for [down] to lift (true, a tap) or to move past the touch slop or be taken by a scroll (false). */
private suspend fun AwaitPointerEventScope.awaitTap(down: PointerInputChange): Boolean {
    while (true) {
        val change = awaitPointerEvent().changes.firstOrNull { it.id == down.id } ?: return false
        if (change.changedToUp()) {
            change.consume()
            return true
        }
        if (change.isConsumed || (change.position - down.position).getDistance() > viewConfiguration.touchSlop) return false
    }
}

/** Where a heading was last placed, kept out of state so scrolling doesn't recompose it. */
private class Placed {
    var coordinates: LayoutCoordinates? = null
}

private val PickerItemHeight = 56.dp

/**
 * The list of sections a held heading opens, shared by the headings (which open it and move
 * the finger) and [SectionPickerOverlay] (which draws it), in window pixels. It opens with the
 * held section under the finger, kept inside [area]. The section level with the finger is
 * [selected], so sideways drift doesn't matter, and past either end is the end one.
 */
@Stable
private class SectionPicker(val itemHeight: Float) {
    var origin by mutableStateOf<Offset?>(null)
        private set
    var fingerY by mutableFloatStateOf(0f)
    var area = Rect.Zero
    private var start = 0
    private var count = 0

    val top: Float
        get() = ((origin?.y ?: 0f) - (start + 0.5f) * itemHeight)
            .coerceAtMost(area.bottom - count * itemHeight)
            .coerceAtLeast(area.top)

    val selected: Int
        get() = floor((fingerY - top) / itemHeight).toInt().coerceIn(0, (count - 1).coerceAtLeast(0))

    fun open(at: Offset, start: Int, count: Int) {
        this.start = start
        this.count = count
        fingerY = at.y
        origin = at
    }

    fun close() {
        origin = null
    }
}

/**
 * [picker]'s list while it's open, over a dimmed screen: across, centred on the finger; down,
 * clear of [topInset] and [bottomInset]. A haptic tick marks each change of section.
 */
@Composable
private fun SectionPickerOverlay(
    picker: SectionPicker,
    sections: List<SectionStart>,
    topInset: Dp,
    bottomInset: Dp,
    modifier: Modifier = Modifier,
) {
    val density = LocalDensity.current
    val placed = remember { Placed() }
    Box(
        modifier.onPlaced {
            placed.coordinates = it
            val bounds = it.boundsInWindow()
            picker.area = with(density) {
                Rect(bounds.left, bounds.top + topInset.toPx(), bounds.right, bounds.bottom - bottomInset.toPx())
            }
        },
    ) {
        val origin = picker.origin ?: return@Box
        val haptics = LocalHapticFeedback.current
        val selected by remember { derivedStateOf { picker.selected } }
        LaunchedEffect(picker) {
            snapshotFlow { picker.selected }.drop(1).collect { haptics.performHapticFeedback(HapticFeedbackType.SegmentTick) }
        }
        Box(Modifier.matchParentSize().background(Color.Black.copy(alpha = 0.32f)))
        Surface(
            shape = RoundedCornerShape(16.dp),
            tonalElevation = 6.dp,
            shadowElevation = 8.dp,
            modifier = Modifier.layout { measurable, constraints ->
                val placeable = measurable.measure(constraints.copy(minWidth = 0, minHeight = 0))
                val corner = placed.coordinates?.localToWindow(Offset.Zero) ?: Offset.Zero
                val margin = Gutter.toPx()
                val x = (origin.x - corner.x - placeable.width / 2)
                    .coerceAtMost(constraints.maxWidth - placeable.width - margin)
                    .coerceAtLeast(margin)
                layout(constraints.maxWidth, constraints.maxHeight) {
                    placeable.place(x.roundToInt(), (picker.top - corner.y).roundToInt())
                }
            },
        ) {
            Column(Modifier.width(IntrinsicSize.Max).widthIn(min = 200.dp)) {
                sections.forEachIndexed { i, start ->
                    val here = i == selected
                    Box(
                        Modifier
                            .fillMaxWidth()
                            .height(PickerItemHeight)
                            .background(if (here) MaterialTheme.colorScheme.secondaryContainer else Color.Transparent)
                            .padding(horizontal = 24.dp),
                        contentAlignment = Alignment.CenterStart,
                    ) {
                        Text(
                            stringResource(start.section.title),
                            style = MaterialTheme.typography.titleMedium,
                            color = if (here) MaterialTheme.colorScheme.onSecondaryContainer else Color.Unspecified,
                            fontWeight = if (here) FontWeight.Bold else null,
                        )
                    }
                }
            }
        }
    }
}

/** What a recent shows under its title: an album's artist, or what kind of thing it is. */
@Composable
internal fun Recent.subtitle(): String = artist ?: stringResource(
    when (ref.kind) {
        CollectionRef.Kind.Artist -> R.string.artist
        CollectionRef.Kind.AlbumGroup -> R.string.album_group
        else -> R.string.playlist
    },
)

/** An artist's albums here, or for one only credited on tracks, their tracks. */
@Composable
internal fun Artist.subtitle(): String =
    if (albumCount > 0) pluralStringResource(R.plurals.albums, albumCount, albumCount)
    else pluralStringResource(R.plurals.tracks, trackCount, trackCount)

/** A list row with art, for recents, playlists and artists. */
@Composable
private fun LibraryRow(
    title: String,
    subtitle: String,
    art: ArtRef,
    artShape: Shape,
    onLongClick: (() -> Unit)? = null,
    onClick: () -> Unit,
) {
    ListItem(
        headlineContent = { Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = { Text(subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        leadingContent = { AlbumArt(art, contentDescription = null, shape = artShape, modifier = Modifier.size(ListArtSize)) },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.combinedClickable(onLongClick = onLongClick, onClick = onClick),
    )
}

/**
 * No music yet: where it can come from, with an import button. While this phone is [inGroup]
 * its sync settings hold nothing, so that's where its music comes from, not pairing.
 */
@Composable
private fun LazyItemScope.EmptyLibrary(
    importPickers: ImportPickers,
    importRunning: Boolean,
    inGroup: Boolean,
    onChooseWhatToSync: () -> Unit,
) {
    Column(
        Modifier.fillParentMaxWidth().fillParentMaxHeight(0.7f).padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Icon(
            Icons.LibraryMusic,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(72.dp),
        )
        Text(
            stringResource(R.string.empty_title),
            style = MaterialTheme.typography.titleLarge,
            modifier = Modifier.padding(top = 16.dp),
        )
        Text(
            stringResource(if (inGroup) R.string.empty_text_choose else R.string.empty_text_import),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(top = 4.dp),
        )
        var menu by remember { mutableStateOf(false) }
        Box(Modifier.padding(top = 24.dp)) {
            Button(onClick = { menu = true }) {
                Icon(Icons.Import, contentDescription = null, modifier = Modifier.padding(end = 8.dp).size(18.dp))
                Text(stringResource(R.string.import_music))
            }
            ImportMenu(menu, importRunning, importPickers, onDismiss = { menu = false })
        }
        if (inGroup) {
            TextButton(onClick = onChooseWhatToSync, modifier = Modifier.padding(top = 8.dp)) {
                Text(stringResource(R.string.choose_what_to_sync))
            }
        }
    }
}
