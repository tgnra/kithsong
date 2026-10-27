package app.musicapp.ui

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.text.format.Formatter
import androidx.activity.compose.LocalActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
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
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.compose.LocalLifecycleOwner
import app.musicapp.AppSettings
import app.musicapp.NativeCore
import app.musicapp.R
import coil3.SingletonImageLoader
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * The app's settings: a way to the devices it syncs with, the permissions it has, the storage
 * it uses, and its version. [libraryDir] is the library folder, whose size is shown. [onSync]
 * opens the sync screen. [onBack] slides the screen away; it is also dismissed by swiping right.
 */
@Composable
fun SettingsScreen(
    settings: AppSettings,
    libraryDir: File,
    bottomPadding: Dp,
    onSync: () -> Unit,
    onBack: () -> Unit,
) {
    val context = LocalContext.current
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            contentPadding = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
                .add(WindowInsets(bottom = bottomPadding))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "header") { ScreenHeader(stringResource(R.string.settings), onBack) }

            item(key = "sync") {
                ListItem(
                    leadingContent = { Icon(Icons.Sync, contentDescription = null) },
                    headlineContent = { Text(stringResource(R.string.sync)) },
                    supportingContent = { Text(stringResource(R.string.settings_sync_text)) },
                    modifier = Modifier.clickable(onClick = onSync),
                )
            }

            // Notifications are the only permission asked for at run time, from API 33.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) item(key = "permissions") {
                SettingsHeading(stringResource(R.string.settings_permissions))
                PermissionRow(
                    Manifest.permission.POST_NOTIFICATIONS,
                    stringResource(R.string.permission_notifications),
                    allowed = stringResource(R.string.permission_notifications_allowed),
                    denied = stringResource(R.string.permission_notifications_denied),
                    asked = { settings.askedForNotifications },
                    onAsk = { settings.askedForNotifications = true },
                )
            }

            item(key = "storage") {
                SettingsHeading(stringResource(R.string.settings_storage))
                val libraryBytes by produceState<Long?>(null) { value = withContext(Dispatchers.IO) { sizeOf(libraryDir) } }
                ListItem(
                    headlineContent = { Text(stringResource(R.string.storage_library)) },
                    supportingContent = {
                        Text(
                            libraryBytes?.let { stringResource(R.string.storage_library_size, Formatter.formatShortFileSize(context, it)) }
                                ?: stringResource(R.string.measuring),
                        )
                    },
                )
                var cleared by remember { mutableIntStateOf(0) }
                val cacheBytes by produceState<Long?>(null, cleared) {
                    value = withContext(Dispatchers.IO) { SingletonImageLoader.get(context).diskCache?.size ?: 0 }
                }
                val scope = rememberCoroutineScope()
                ListItem(
                    headlineContent = { Text(stringResource(R.string.storage_art_cache)) },
                    supportingContent = {
                        Text(
                            cacheBytes?.let { stringResource(R.string.storage_art_cache_size, Formatter.formatShortFileSize(context, it)) }
                                ?: stringResource(R.string.measuring),
                        )
                    },
                    trailingContent = {
                        TextButton(
                            onClick = {
                                scope.launch {
                                    withContext(Dispatchers.IO) { context.clearArtCache() }
                                    cleared++
                                }
                            },
                            enabled = (cacheBytes ?: 0) > 0,
                        ) { Text(stringResource(R.string.clear)) }
                    },
                )
            }

            item(key = "about") {
                SettingsHeading(stringResource(R.string.settings_about))
                val version = remember { context.packageManager.getPackageInfo(context.packageName, 0).versionName }
                ListItem(
                    headlineContent = { Text(stringResource(R.string.app_name)) },
                    supportingContent = { Text(stringResource(R.string.version, version.orEmpty())) },
                )
                ListItem(
                    headlineContent = { Text(stringResource(R.string.core_name)) },
                    supportingContent = { Text(stringResource(R.string.version, NativeCore.version())) },
                )
            }
        }
    }
}

/**
 * A full screen's header: a back button calling [onBack], and [title] under it, or beside it
 * in a short window. [actions] go at the end of the back button's row.
 */
@Composable
fun ScreenHeader(title: String, onBack: () -> Unit, actions: @Composable RowScope.() -> Unit = {}) {
    val back = @Composable {
        IconButton(onClick = onBack, modifier = Modifier.padding(start = 4.dp, top = 4.dp)) {
            Icon(Icons.ArrowBack, contentDescription = stringResource(R.string.back))
        }
    }
    val heading = @Composable { modifier: Modifier ->
        Text(
            title,
            style = MaterialTheme.typography.headlineMedium,
            modifier = modifier.semantics { heading() },
        )
    }
    if (isShortWindow()) {
        Row(
            Modifier.fillMaxWidth().windowInsetsPadding(WindowInsets.statusBars),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            back()
            heading(Modifier.weight(1f).padding(start = 8.dp, top = 4.dp))
            actions()
        }
    } else {
        Column(Modifier.fillMaxWidth().windowInsetsPadding(WindowInsets.statusBars)) {
            Row(Modifier.fillMaxWidth().padding(end = 4.dp), verticalAlignment = Alignment.CenterVertically) {
                back()
                Spacer(Modifier.weight(1f))
                actions()
            }
            heading(Modifier.padding(horizontal = 16.dp, vertical = 8.dp))
        }
    }
}

@Composable
fun SettingsHeading(title: String) {
    Text(
        title,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 16.dp, end = 16.dp, top = 24.dp, bottom = 4.dp).semantics { heading() },
    )
}

/**
 * Whether [permission] is granted, checked again whenever the app comes back to the front
 * (it may have been changed in the system settings). Tapping asks for it while the system
 * still allows that, and otherwise opens the app's system settings, where it can be changed.
 * [asked] is whether it has been asked for before, and [onAsk] notes that it's being asked for.
 */
@Composable
private fun PermissionRow(
    permission: String,
    title: String,
    allowed: String,
    denied: String,
    asked: () -> Boolean,
    onAsk: () -> Unit,
) {
    val context = LocalContext.current
    val activity = LocalActivity.current
    var granted by remember { mutableStateOf(context.isGranted(permission)) }
    val lifecycle = LocalLifecycleOwner.current.lifecycle
    DisposableEffect(lifecycle) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_RESUME) granted = context.isGranted(permission)
        }
        lifecycle.addObserver(observer)
        onDispose { lifecycle.removeObserver(observer) }
    }
    val request = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) { granted = it }
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = { Text(if (granted) allowed else denied) },
        modifier = Modifier.clickable(onClickLabel = stringResource(if (granted) R.string.open_app_settings else R.string.allow)) {
            val canAsk = !asked() || activity?.let { ActivityCompat.shouldShowRequestPermissionRationale(it, permission) } == true
            if (!granted && canAsk) {
                onAsk()
                request.launch(permission)
            } else {
                context.openAppSettings()
            }
        },
    )
}

private fun Context.isGranted(permission: String) =
    ContextCompat.checkSelfPermission(this, permission) == PackageManager.PERMISSION_GRANTED

/** The system's settings page for this app, where its permissions can be changed. */
private fun Context.openAppSettings() {
    startActivity(
        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", packageName, null))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
    )
}

private fun Context.clearArtCache() {
    val loader = SingletonImageLoader.get(this)
    loader.memoryCache?.clear()
    loader.diskCache?.clear()
}

private fun sizeOf(dir: File): Long = dir.walkBottomUp().filter { it.isFile }.sumOf { it.length() }
