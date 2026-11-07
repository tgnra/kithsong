package ng.kithso.android.ui

import android.text.format.DateUtils
import android.widget.Toast
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.RecycleBin
import ng.kithso.core.BinEntry
import ng.kithso.core.DeleteKind
import kotlinx.coroutines.launch

/**
 * The recycle bin: what was deleted on this phone since the last sync, newest first, each with
 * Restore. Its files are gone from this phone already; restoring keeps it in the library and on
 * the other devices, removed from this phone only. The list is read again whenever [binCount]
 * changes (a delete, a restore, or a sync that sent them all). [onBack] slides the screen away;
 * it is also dismissed by swiping right.
 */
@Composable
fun RecycleBinScreen(bin: RecycleBin, binCount: Int, bottomPadding: Dp, onBack: () -> Unit) {
    val context = LocalContext.current.applicationContext
    val scope = rememberCoroutineScope()
    var reads by remember { mutableIntStateOf(0) }
    var restoring by remember { mutableStateOf<Long?>(null) }
    val entries by produceState<List<BinEntry>?>(null, binCount, reads) { value = bin.binEntries() }
    val untitled = stringResource(R.string.untitled)
    val failed = stringResource(R.string.recycle_bin_restore_failed)
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            contentPadding = pageSides()
                .add(WindowInsets(bottom = bottomPadding))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "header") {
                ScreenHeader(stringResource(R.string.recycle_bin), onBack)
                Text(
                    stringResource(R.string.recycle_bin_text),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
                )
            }
            val list = entries
            if (list != null && list.isEmpty()) item(key = "empty") {
                Text(
                    stringResource(R.string.recycle_bin_empty),
                    style = MaterialTheme.typography.bodyLarge,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 24.dp),
                )
            }
            items(list.orEmpty(), key = { it.id }) { entry ->
                val title = entry.title?.takeIf { it.isNotEmpty() } ?: untitled
                ListItem(
                    leadingContent = {
                        Icon(if (entry.kind == DeleteKind.Album) Icons.Album else Icons.LibraryMusic, contentDescription = null)
                    },
                    headlineContent = { Text(title) },
                    supportingContent = { Text(entryText(entry)) },
                    trailingContent = {
                        TextButton(
                            enabled = restoring == null,
                            onClick = {
                                restoring = entry.id
                                scope.launch {
                                    val came = bin.binRestore(entry)
                                    restoring = null
                                    reads++
                                    val text = when {
                                        came == null -> failed
                                        came > 1 -> context.resources.getQuantityString(
                                            R.plurals.recycle_bin_restored_more, came - 1, title, came - 1,
                                        )
                                        else -> context.getString(R.string.recycle_bin_restored, title)
                                    }
                                    Toast.makeText(context, text, Toast.LENGTH_LONG).show()
                                }
                            },
                        ) { Text(stringResource(R.string.recycle_bin_restore)) }
                    },
                )
            }
        }
    }
}

// Its artist, how many tracks, and when it was deleted.
@Composable
private fun entryText(entry: BinEntry): String {
    val now = System.currentTimeMillis()
    val ago = if (now - entry.deletedMs < DateUtils.MINUTE_IN_MILLIS) {
        stringResource(R.string.synced_just_now)
    } else {
        DateUtils.getRelativeTimeSpanString(entry.deletedMs, now, DateUtils.MINUTE_IN_MILLIS).toString()
    }
    return listOfNotNull(
        entry.artist?.takeIf { it.isNotEmpty() },
        pluralStringResource(R.plurals.tracks, entry.tracks, entry.tracks),
        stringResource(R.string.recycle_bin_deleted_ago, ago),
    ).joinToString(stringResource(R.string.separator_dot))
}
