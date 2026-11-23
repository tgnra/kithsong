package ng.kithso.android.archive

import android.content.ActivityNotFoundException
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import ng.kithso.android.R
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Settings' Export library: picks where the zip goes (which may be a USB drive), then exports
 * to it, showing how far it has got, with Cancel, and then how it went.
 */
@Composable
fun ExportLibraryRow(archiver: LibraryArchiver) {
    val context = LocalContext.current
    val job by archiver.job.collectAsState()
    val export = job?.takeIf { !it.restoring }
    val create = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        if (uri != null) archiver.export(uri)
    }
    val fileName = stringResource(R.string.library_export_file, SimpleDateFormat("yyyy-MM-dd", Locale.ROOT).format(Date()))
    ListItem(
        headlineContent = { Text(stringResource(R.string.library_export)) },
        supportingContent = {
            Column {
                Text(export?.title(context) ?: stringResource(R.string.library_export_text))
                if (export?.isOver == false) {
                    LinearProgressIndicator(
                        progress = { export.percent / 100f },
                        modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
                    )
                }
            }
        },
        trailingContent = if (export?.isOver == false) {
            { TextButton(onClick = archiver::cancel) { Text(stringResource(R.string.cancel)) } }
        } else {
            null
        },
        modifier = Modifier.clickable(enabled = job?.isOver != false) {
            archiver.dismiss()
            try {
                create.launch(fileName)
            } catch (e: ActivityNotFoundException) {
                Log.w("kithsong", "No document picker", e)
            }
        },
    )
}

/**
 * A library export picked to import into the empty library is restored as the whole library:
 * this shows how far it has got, with Cancel, and then what went wrong if it did; once it's
 * done the app starts again with the library.
 */
@Composable
fun RestoreDialog(archiver: LibraryArchiver) {
    val context = LocalContext.current
    val job by archiver.job.collectAsState()
    val restore = job?.takeIf { it.restoring } ?: return
    val running = !restore.isOver
    AlertDialog(
        onDismissRequest = { if (!running) archiver.dismiss() },
        properties = DialogProperties(dismissOnClickOutside = false),
        title = { Text(restore.title(context)) },
        text = {
            if (running || restore.result == ArchiveJob.Result.Done) {
                Column {
                    Text(stringResource(R.string.library_restoring_text))
                    LinearProgressIndicator(
                        progress = { restore.percent / 100f },
                        modifier = Modifier.fillMaxWidth().padding(top = 16.dp),
                    )
                }
            }
        },
        confirmButton = {
            if (running) {
                TextButton(onClick = archiver::cancel) { Text(stringResource(R.string.cancel)) }
            } else if (restore.result != ArchiveJob.Result.Done) {
                TextButton(onClick = archiver::dismiss) { Text(stringResource(R.string.ok)) }
            }
        },
    )
}
