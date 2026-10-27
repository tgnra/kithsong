package app.musicapp.ui

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import android.net.Uri
import android.provider.Settings
import androidx.activity.compose.LocalActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import app.musicapp.AppSettings
import app.musicapp.R
import app.musicapp.importer.DeviceMusic
import app.musicapp.importer.ImportProgress
import app.musicapp.importer.Importer
import app.musicapp.importer.MusicFolder
import app.musicapp.importer.detail
import app.musicapp.importer.title
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/** Where the import button has got to: asking for access, then choosing folders. */
@Immutable
private sealed interface ImportStep {
    data object Checking : ImportStep
    data object Running : ImportStep
    data object Rationale : ImportStep
    data object Asking : ImportStep
    data object Denied : ImportStep
    data object Loading : ImportStep
    data class Folders(val folders: List<MusicFolder>) : ImportStep
}

/**
 * What the import button opens: the audio permission if it's needed (with a rationale when
 * the system suggests one, and a way to the app's settings once it has been denied for
 * good), then a sheet of the folders of music on the phone. [onStarted] is called once an
 * import starts, and [onClose] when there's nothing more to show.
 */
@Composable
fun ImportFlow(importer: Importer, settings: AppSettings, onStarted: () -> Unit, onClose: () -> Unit) {
    val context = LocalContext.current
    val activity = LocalActivity.current
    var step by remember { mutableStateOf<ImportStep>(ImportStep.Checking) }
    val request = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        when {
            granted -> step = ImportStep.Loading
            // Denied with nothing to explain: it won't be asked again, so only settings can help.
            activity != null && !activity.shouldShowAudioRationale() -> step = ImportStep.Denied
            else -> onClose()
        }
    }
    val ask = {
        settings.askedForAudio = true
        step = ImportStep.Asking
        request.launch(DeviceMusic.AUDIO_PERMISSION)
    }

    LaunchedEffect(Unit) {
        when {
            importer.isRunning -> step = ImportStep.Running
            context.hasAudioPermission() -> step = ImportStep.Loading
            activity?.shouldShowAudioRationale() == true -> step = ImportStep.Rationale
            settings.askedForAudio -> step = ImportStep.Denied
            else -> ask()
        }
    }
    LaunchedEffect(step) {
        if (step == ImportStep.Loading) step = ImportStep.Folders(withContext(Dispatchers.IO) { DeviceMusic.folders(context) })
    }

    when (val current = step) {
        ImportStep.Checking, ImportStep.Asking -> Unit
        ImportStep.Running -> AlertDialog(
            onDismissRequest = onClose,
            title = { Text(stringResource(R.string.import_running_title)) },
            text = { Text(stringResource(R.string.import_running_text)) },
            confirmButton = { TextButton(onClick = onClose) { Text(stringResource(R.string.ok)) } },
        )
        ImportStep.Rationale -> AlertDialog(
            onDismissRequest = onClose,
            title = { Text(stringResource(R.string.import_rationale_title)) },
            text = { Text(stringResource(R.string.import_rationale_text)) },
            confirmButton = { TextButton(onClick = ask) { Text(stringResource(R.string.import_continue)) } },
            dismissButton = { TextButton(onClick = onClose) { Text(stringResource(R.string.not_now)) } },
        )
        ImportStep.Denied -> AlertDialog(
            onDismissRequest = onClose,
            title = { Text(stringResource(R.string.import_denied_title)) },
            text = { Text(stringResource(R.string.import_denied_text)) },
            confirmButton = {
                TextButton(onClick = {
                    context.openAppSettings()
                    onClose()
                }) { Text(stringResource(R.string.open_settings)) }
            },
            dismissButton = { TextButton(onClick = onClose) { Text(stringResource(R.string.cancel)) } },
        )
        ImportStep.Loading -> FolderSheet(null, settings, onImport = {}, onClose)
        is ImportStep.Folders -> FolderSheet(
            current.folders,
            settings,
            onImport = { chosen ->
                if (importer.start(chosen.flatMap { it.files }, settings.fetchArt)) {
                    settings.importedFolders += chosen.map { it.path }
                    onStarted()
                }
                onClose()
            },
            onClose,
        )
    }
}

/**
 * The folders of music to choose from, or a spinner while [folders] is null. Folders not
 * imported before start ticked.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun FolderSheet(
    folders: List<MusicFolder>?,
    settings: AppSettings,
    onImport: (List<MusicFolder>) -> Unit,
    onClose: () -> Unit,
) {
    val context = LocalContext.current
    val imported = remember { settings.importedFolders }
    var chosen by remember(folders) { mutableStateOf(folders.orEmpty().filter { it.path !in imported }.map { it.path }.toSet()) }
    val offline = remember { !context.isOnline() }

    ModalBottomSheet(onDismissRequest = onClose) {
        Column(Modifier.navigationBarsPadding()) {
            Text(
                stringResource(R.string.import_sheet_title),
                style = MaterialTheme.typography.titleLarge,
                modifier = Modifier.padding(horizontal = 24.dp),
            )
            Text(
                stringResource(R.string.import_sheet_text),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(horizontal = 24.dp, vertical = 8.dp),
            )
            if (offline) {
                Text(
                    stringResource(R.string.import_offline),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.error,
                    modifier = Modifier.padding(horizontal = 24.dp, vertical = 4.dp),
                )
            }
            when {
                folders == null -> Box(Modifier.fillMaxWidth().padding(32.dp), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator()
                }
                folders.isEmpty() -> Text(
                    stringResource(R.string.import_no_folders),
                    style = MaterialTheme.typography.bodyLarge,
                    modifier = Modifier.padding(24.dp),
                )
                else -> LazyColumn(Modifier.heightIn(max = 420.dp)) {
                    items(folders, key = { it.path }) { folder ->
                        val checked = folder.path in chosen
                        ListItem(
                            headlineContent = { Text(folder.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                            supportingContent = {
                                val tracks = pluralStringResource(R.plurals.tracks, folder.files.size, folder.files.size)
                                val format = if (folder.path in imported) R.string.import_folder_before else R.string.import_folder
                                Text(stringResource(format, tracks, folder.volume))
                            },
                            leadingContent = { Checkbox(checked = checked, onCheckedChange = null) },
                            colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                            modifier = Modifier.toggleable(checked, role = Role.Checkbox) {
                                chosen = if (it) chosen + folder.path else chosen - folder.path
                            },
                        )
                    }
                }
            }
            Row(
                Modifier.fillMaxWidth().padding(horizontal = 24.dp, vertical = 16.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End),
            ) {
                TextButton(onClick = onClose) { Text(stringResource(R.string.cancel)) }
                val selected = folders.orEmpty().filter { it.path in chosen }
                Button(onClick = { onImport(selected) }, enabled = selected.isNotEmpty()) {
                    val count = selected.sumOf { it.files.size }
                    Text(
                        if (selected.isEmpty()) stringResource(R.string.import_button)
                        else pluralStringResource(R.plurals.import_button_tracks, count, count),
                    )
                }
            }
        }
    }
}

/**
 * How the import is going, under the main screen's header: what it's doing, a progress bar,
 * and a way to cancel; then the items with no sure match, to import as tagged or skip; then
 * a summary with what failed, until dismissed.
 */
@Composable
fun ImportCard(progress: ImportProgress, importer: Importer, modifier: Modifier = Modifier) {
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
                ImportProgress.Phase.Scanning -> LinearProgressIndicator(bar)
                ImportProgress.Phase.Matching ->
                    LinearProgressIndicator(progress = { progress.settled.toFloat() / progress.items.coerceAtLeast(1) }, modifier = bar)
                else -> Unit
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
            Row(
                Modifier.fillMaxWidth().padding(vertical = 4.dp),
                horizontalArrangement = Arrangement.spacedBy(4.dp, Alignment.End),
            ) {
                when (progress.phase) {
                    ImportProgress.Phase.Scanning, ImportProgress.Phase.Matching ->
                        TextButton(onClick = importer::cancel) { Text(stringResource(R.string.cancel)) }
                    ImportProgress.Phase.Review -> {
                        TextButton(onClick = importer::skipAll) { Text(stringResource(R.string.import_skip)) }
                        TextButton(onClick = importer::acceptAllAsIs) { Text(stringResource(R.string.import_as_tagged)) }
                    }
                    ImportProgress.Phase.Finished, ImportProgress.Phase.Cancelled ->
                        TextButton(onClick = importer::dismiss) { Text(stringResource(R.string.done)) }
                }
            }
        }
    }
}

private const val MAX_PROBLEMS = 3

fun Context.hasAudioPermission() =
    ContextCompat.checkSelfPermission(this, DeviceMusic.AUDIO_PERMISSION) == PackageManager.PERMISSION_GRANTED

private fun Activity.shouldShowAudioRationale() =
    ActivityCompat.shouldShowRequestPermissionRationale(this, DeviceMusic.AUDIO_PERMISSION)

/** The system's settings page for this app, where its permissions can be changed. */
fun Context.openAppSettings() {
    startActivity(
        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", packageName, null))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
    )
}

private fun Context.isOnline(): Boolean {
    val connectivity = getSystemService(ConnectivityManager::class.java)
    val capabilities = connectivity.getNetworkCapabilities(connectivity.activeNetwork) ?: return false
    return capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
}
