package ng.kithso.android.ui

import android.content.ActivityNotFoundException
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import ng.kithso.android.KithsongApp
import ng.kithso.android.NewLibrary
import ng.kithso.android.R
import ng.kithso.android.exportProgressText
import ng.kithso.android.sync.Pairing
import ng.kithso.android.sync.Syncer
import ng.kithso.core.CoreSync
import ng.kithso.core.NearbyDevice

/** What a library mismatch's dialog offers ([NewLibraryDialogs]), for its device [NewLibrary.Target]. */
@Stable
class NewLibraryActions(
    /** Whether this phone could have joined: it's in no group, so a new, empty library lets it. */
    val canStart: Boolean,
    /** Export and start new: picks a folder, exports into it, then asks to start the new library. */
    val export: (NewLibrary.Target) -> Unit,
    /** Start new without exporting: asks first, as the library is deleted. */
    val startWithoutExport: (NewLibrary.Target) -> Unit,
)

/**
 * The phone's Create a new library ([NewLibrary]), from a pairing refused because both devices
 * have music: the folder picker for the export, the export's progress (with Cancel), how it
 * went, and Start new library once it's done; or, without exporting, the question that says
 * the library will be deleted. After the restart into the new library, Pair again. Shown over
 * everything, with the pairing dialogs; the returned actions are for the mismatch's dialog.
 */
@Composable
fun NewLibraryDialogs(syncer: Syncer): NewLibraryActions {
    val app = LocalContext.current.applicationContext as KithsongApp
    val newLibrary = app.newLibrary
    val status by syncer.status.collectAsState()
    // The device the export or question is for, kept through the picker and turning the phone.
    var exportFor by rememberSaveable { mutableStateOf<String?>(null) }
    var confirmFor by rememberSaveable { mutableStateOf<String?>(null) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { tree ->
        val target = exportFor?.let(NewLibrary.Target::decode)
        exportFor = null
        if (tree != null) newLibrary.export(tree, target)
    }

    confirmFor?.let(NewLibrary.Target::decode)?.let { target ->
        AlertDialog(
            onDismissRequest = { confirmFor = null },
            title = { Text(stringResource(R.string.new_library_delete_title)) },
            text = { Text(stringResource(R.string.new_library_delete_text)) },
            confirmButton = {
                TextButton(onClick = {
                    confirmFor = null
                    newLibrary.start(target)
                }) { Text(stringResource(R.string.new_library_delete), color = MaterialTheme.colorScheme.error) }
            },
            dismissButton = { TextButton(onClick = { confirmFor = null }) { Text(stringResource(R.string.cancel)) } },
        )
    }

    val state by newLibrary.state.collectAsState()
    when (val export = state) {
        NewLibrary.State.Idle -> Unit
        is NewLibrary.State.Exporting -> ExportDialog(export, onCancel = newLibrary::cancel)
        is NewLibrary.State.Exported -> AlertDialog(
            onDismissRequest = {},
            title = { Text(stringResource(R.string.export_done_title)) },
            text = {
                Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Text(
                        if (export.zips > 0) pluralStringResource(R.plurals.export_done_text, export.zips, export.zips)
                        else stringResource(R.string.export_done_nothing),
                    )
                    if (export.tooBig > 0) Text(pluralStringResource(R.plurals.export_too_big, export.tooBig, export.tooBig))
                    Text(stringResource(R.string.export_start_new))
                }
            },
            confirmButton = {
                TextButton(onClick = { newLibrary.start(export.target) }) { Text(stringResource(R.string.new_library_start)) }
            },
            // The library stays, and so do the zips.
            dismissButton = { TextButton(onClick = newLibrary::dismiss) { Text(stringResource(R.string.cancel)) } },
        )
        is NewLibrary.State.Failed -> AlertDialog(
            onDismissRequest = newLibrary::dismiss,
            title = { Text(stringResource(R.string.export_failed_title)) },
            text = {
                Text(
                    export.readPath?.let { stringResource(R.string.export_failed_read, it) }
                        ?: stringResource(R.string.export_failed_text),
                )
            },
            confirmButton = { TextButton(onClick = newLibrary::dismiss) { Text(stringResource(R.string.ok)) } },
        )
    }

    // In the new library: asks the device again once sync is running, by uid or address.
    val again by newLibrary.pairAgain.collectAsState()
    again?.takeIf { status.running && state == NewLibrary.State.Idle && status.pairing == null }?.let { target ->
        AlertDialog(
            onDismissRequest = newLibrary::pairAgainAnswered,
            title = { Text(stringResource(R.string.new_library_ready_title)) },
            text = { Text(stringResource(R.string.pair_again_text, target.name)) },
            confirmButton = {
                TextButton(onClick = {
                    newLibrary.pairAgainAnswered()
                    syncer.pairAgain(target.uid, target.name, target.host, target.port)
                }) { Text(stringResource(R.string.pair_again)) }
            },
            dismissButton = { TextButton(onClick = newLibrary::pairAgainAnswered) { Text(stringResource(R.string.cancel)) } },
        )
    }

    // As could_join on the desktops; never in a stress library, which doesn't sync, nor in the
    // demo library, which isn't the phone's own.
    val canStart = status.libraryState == CoreSync.LibraryState.NoGroup && !app.testing.inStress && !app.testing.inDemo
    val latestPicker by rememberUpdatedState(picker)
    return remember(canStart) {
        NewLibraryActions(
            canStart = canStart,
            export = { target ->
                exportFor = target.encode()
                try {
                    latestPicker.launch(null)
                } catch (e: ActivityNotFoundException) {
                    // No documents UI, as on some stripped-down devices.
                    Log.w("kithsong", "No picker", e)
                    exportFor = null
                }
            },
            startWithoutExport = { target -> confirmFor = target.encode() },
        )
    }
}

/** The export under way: how far, with Cancel, which deletes what it wrote and keeps the library. */
@Composable
private fun ExportDialog(export: NewLibrary.State.Exporting, onCancel: () -> Unit) {
    val context = LocalContext.current
    val progress = export.progress
    AlertDialog(
        onDismissRequest = {},
        title = { Text(stringResource(R.string.export_title)) },
        text = {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                if (progress == null) {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                } else {
                    LinearProgressIndicator(progress = { progress.fraction }, modifier = Modifier.fillMaxWidth())
                }
                Text(
                    progress?.let { exportProgressText(context, it) } ?: stringResource(R.string.export_listing),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Text(stringResource(R.string.export_unchanged))
            }
        },
        confirmButton = { TextButton(onClick = onCancel) { Text(stringResource(R.string.cancel)) } },
    )
}

/**
 * The device to pair with again, from how the pairing ended: by uid, at the address discovery
 * has for it now, else where this phone asked it.
 */
fun Pairing.Ended.newLibraryTarget(nearby: List<NearbyDevice>): NewLibrary.Target {
    val seen = nearby.firstOrNull { peerUid.isNotEmpty() && it.uid == peerUid && it.address != null }
    return if (seen != null) {
        NewLibrary.Target(peerUid, peer, seen.address.orEmpty(), seen.port)
    } else {
        NewLibrary.Target(peerUid, peer, peerHost, peerPort)
    }
}
