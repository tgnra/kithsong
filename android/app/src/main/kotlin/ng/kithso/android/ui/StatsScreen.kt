package ng.kithso.android.ui

import android.text.format.Formatter
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import java.text.NumberFormat
import ng.kithso.android.R
import ng.kithso.android.data.LibrarySource
import ng.kithso.core.LibraryStats
import ng.kithso.core.LibraryTally

/**
 * Numbers about the library: what's on this phone, the whole library's if some of it isn't here,
 * its lists, and how much of it has been played. Read off the main thread, and again whenever
 * the library changes, as a sync can while this is open. [onBack] slides the screen away; it is
 * also dismissed by swiping right.
 */
@Composable
fun StatsScreen(source: LibrarySource, bottomPadding: Dp, onBack: () -> Unit) {
    val library by source.library.collectAsState()
    // Null only until first read: just the header, rather than a flash of the empty state.
    val stats by produceState<LibraryStats?>(null, library) { value = source.stats() }
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            contentPadding = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal).inPane()
                .add(WindowInsets(bottom = bottomPadding))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "header") { ScreenHeader(stringResource(R.string.stats_title), onBack) }
            val s = stats
            if (s != null && s.all.tracks == 0L) {
                item(key = "empty") { EmptyStats(Modifier.fillParentMaxHeight(0.7f)) }
            } else if (s != null) {
                item(key = "here") {
                    SettingsHeading(stringResource(R.string.stats_here))
                    TallyRows(s.here)
                    StatRow(stringResource(R.string.stats_size), Formatter.formatShortFileSize(LocalContext.current, s.sizeBytes))
                }
                // The same numbers twice say nothing when all of it is here.
                if (s.all != s.here) item(key = "all") {
                    SettingsHeading(stringResource(R.string.stats_all))
                    TallyRows(s.all)
                }
                item(key = "lists") {
                    SettingsHeading(stringResource(R.string.stats_lists))
                    StatRow(stringResource(R.string.section_playlists), count(s.playlists + s.smartPlaylists))
                    StatRow(stringResource(R.string.section_album_groups), count(s.albumGroups + s.smartAlbumGroups))
                }
                item(key = "listening") {
                    SettingsHeading(stringResource(R.string.stats_listening))
                    StatRow(stringResource(R.string.stats_plays), count(s.plays))
                    StatRow(stringResource(R.string.stats_tracks_played), count(s.tracksPlayed))
                    StatRow(stringResource(R.string.stats_time_listened), playingTime(s.listenedMs))
                }
            }
        }
    }
}

@Composable
private fun TallyRows(tally: LibraryTally) {
    StatRow(stringResource(R.string.tracks_title), count(tally.tracks))
    StatRow(stringResource(R.string.section_albums), count(tally.albums))
    StatRow(stringResource(R.string.section_singles), count(tally.singles))
    StatRow(stringResource(R.string.section_artists), count(tally.artists))
    StatRow(stringResource(R.string.stats_genres), count(tally.genres))
    StatRow(stringResource(R.string.stats_playing_time), playingTime(tally.durationMs))
}

/** A number and what it counts, read out as one. */
@Composable
private fun StatRow(label: String, value: String) {
    ListItem(
        headlineContent = { Text(label) },
        trailingContent = { Text(value, style = MaterialTheme.typography.bodyLarge) },
        modifier = Modifier.semantics(mergeDescendants = true) {},
    )
}

@Composable
private fun EmptyStats(modifier: Modifier) {
    Column(
        modifier.fillMaxWidth().padding(32.dp),
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
            stringResource(R.string.stats_empty_text),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(top = 4.dp),
        )
    }
}

/** [n] in the language's digits and grouping. */
private fun count(n: Long): String = NumberFormat.getIntegerInstance().format(n)

