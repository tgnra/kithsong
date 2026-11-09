package ng.kithso.android.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.gestures.scrollBy
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalClipboardManager
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ng.kithso.android.KithsongApp
import ng.kithso.android.R
import ng.kithso.core.CoreTesting
import ng.kithso.core.CoreEdit

/** The scroll test's script: the main list, then the biggest album's tracks. */
object ScrollTest {
    enum class Step { Idle, Main, OpenTracks, Tracks, Done }

    val step = MutableStateFlow(Step.Idle)

    fun start() {
        if (!CoreTesting.enabled() || step.value != Step.Idle) return
        CoreTesting.autoBegin()
        step.value = Step.Main
    }
}

/**
 * Scrolls [state] by the core's fixed script, one step per frame, each frame reported under
 * [series]. It stops early at the end of the list.
 */
suspend fun runScrollScript(state: LazyListState, pxPerFrame: Float, series: String) {
    state.scrollToItem(0)
    CoreTesting.frameBreak(series)
    repeat(CoreTesting.SCROLL_FRAMES) {
        withFrameNanos {}
        val moved = state.scrollBy(pxPerFrame)
        CoreTesting.frame(series)
        if (moved == 0f) {
            CoreTesting.frameBreak(series)
            return
        }
    }
    CoreTesting.frameBreak(series)
}

/** What a scroll step comes to on this screen, in pixels. */
@Composable
fun scrollStepPx(): Float = with(LocalDensity.current) { CoreTesting.SCROLL_DP_PER_FRAME.dp.toPx() }

/** Reports [state]'s frames under [series] while the user scrolls it and the profiler's on. */
@Composable
fun ScrollFrames(state: LazyListState, series: String) {
    LaunchedEffect(state, series) {
        snapshotFlow { state.isScrollInProgress }.collect { scrolling ->
            if (scrolling && CoreTesting.enabled() && !CoreTesting.autoRunning()) {
                while (state.isScrollInProgress) {
                    withFrameNanos {}
                    CoreTesting.frame(series)
                }
                CoreTesting.frameBreak(series)
            }
        }
    }
}

/** A card under the main list's header while a stress test library is open: the way back. */
@Composable
fun StressCard() {
    val testing = (LocalContext.current.applicationContext as KithsongApp).testing
    Card(Modifier.fillMaxWidth()) {
        Row(Modifier.padding(start = 16.dp, end = 8.dp), verticalAlignment = Alignment.CenterVertically) {
            Text(
                stringResource(R.string.stress_banner_title),
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.weight(1f),
            )
            TextButton(onClick = testing::back) { Text(stringResource(R.string.stress_back)) }
        }
    }
}

/**
 * Settings' Testing section, closed to begin with, unless a stress library is open or a scroll
 * test has just ended: the way back and the report are in it. [onRunTest] closes Settings for the
 * test to run.
 */
@Composable
fun TestingSettings(onRunTest: () -> Unit) {
    val context = LocalContext.current
    val app = context.applicationContext as KithsongApp
    val testing = app.testing
    val state by testing.state.collectAsState()
    var open by rememberSaveable { mutableStateOf(testing.inStress || CoreTesting.autoRunning() || TestingOpen.next) }
    TestingOpen.next = false
    ListItem(
        headlineContent = { Text(stringResource(R.string.testing_heading)) },
        trailingContent = { Text(if (open) "▴" else "▾") },
        modifier = Modifier.toggleable(open, role = Role.DropdownList) { open = it },
    )
    if (!open) return

    var profiling by remember { mutableStateOf(CoreTesting.enabled()) }
    ListItem(
        headlineContent = { Text(stringResource(R.string.perf_enable)) },
        supportingContent = { Text(stringResource(R.string.perf_enable_text)) },
        trailingContent = { Switch(checked = profiling, onCheckedChange = null) },
        modifier = Modifier.toggleable(profiling, role = Role.Switch) { on ->
            testing.setProfiling(on)
            profiling = on
        },
    )
    var reportVersion by remember { mutableIntStateOf(0) }
    if (profiling) {
        ListItem(
            headlineContent = { Text(stringResource(R.string.perf_run_scroll)) },
            modifier = Modifier.clickable(onClick = onRunTest),
        )
        val clipboard = LocalClipboardManager.current
        val scope = rememberCoroutineScope()
        val saver = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("text/plain")) { uri ->
            if (uri != null) scope.launch(Dispatchers.IO) {
                runCatching { context.contentResolver.openOutputStream(uri)?.use { it.write(CoreTesting.report().toByteArray()) } }
            }
        }
        ListItem(
            headlineContent = { Text(stringResource(R.string.perf_copy)) },
            modifier = Modifier.clickable { clipboard.setText(AnnotatedString(CoreTesting.report())) },
        )
        ListItem(
            headlineContent = { Text(stringResource(R.string.perf_save)) },
            modifier = Modifier.clickable { saver.launch("kithsong-profile.txt") },
        )
        ListItem(
            headlineContent = { Text(stringResource(R.string.perf_clear)) },
            modifier = Modifier.clickable {
                CoreTesting.clear()
                reportVersion++
            },
        )
    }

    var preset by rememberSaveable { mutableIntStateOf(CoreTesting.PRESET_10K) }
    SettingsHeading(stringResource(R.string.stress_heading))
    for (i in 0 until CoreTesting.PRESETS) {
        val tracks = CoreTesting.presetTracks(i)
        ListItem(
            headlineContent = { Text(stringResource(R.string.stress_preset, tracks)) },
            leadingContent = { RadioButton(selected = preset == i, onClick = null) },
            modifier = Modifier.selectable(preset == i, role = Role.RadioButton) { preset = i },
        )
    }
    val generating = state.percent
    ListItem(
        headlineContent = { Text(stringResource(if (generating != null) R.string.cancel else R.string.stress_generate)) },
        supportingContent = when {
            generating != null -> ({ Text(stringResource(R.string.stress_generating, generating)) })
            state.failure != null -> ({ Text(stringResource(R.string.stress_failed, CoreEdit.statusText(state.failure!!))) })
            else -> null
        },
        modifier = Modifier.clickable { if (generating != null) testing.cancel() else testing.generate(preset) },
    )
    if (testing.inStress || testing.inDemo) ListItem(
        headlineContent = { Text(stringResource(R.string.stress_back)) },
        modifier = Modifier.clickable(onClick = testing::back),
    )
    var confirming by remember { mutableStateOf(false) }
    if (!testing.inStress && state.libraries > 0) ListItem(
        headlineContent = { Text(stringResource(R.string.stress_delete)) },
        supportingContent = if (state.deleting) ({ Text(stringResource(R.string.stress_deleting)) }) else null,
        modifier = Modifier.clickable(enabled = !state.deleting) { confirming = true },
    )
    state.deleted?.let {
        ListItem(headlineContent = { Text(pluralStringResource(R.plurals.stress_deleted, it, it)) })
    }
    if (confirming) AlertDialog(
        onDismissRequest = { confirming = false },
        text = { Text(stringResource(R.string.stress_delete_confirm)) },
        confirmButton = {
            TextButton(onClick = {
                confirming = false
                testing.deleteAll()
            }) { Text(stringResource(R.string.delete)) }
        },
        dismissButton = { TextButton(onClick = { confirming = false }) { Text(stringResource(R.string.cancel)) } },
    )
    Text(
        stringResource(R.string.stress_text),
        style = MaterialTheme.typography.bodySmall,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
    )

    // The report, read again now and then as what's measured goes on.
    var report by remember { mutableStateOf("") }
    LaunchedEffect(profiling, reportVersion) {
        while (true) {
            report = if (profiling) withContext(Dispatchers.Default) { CoreTesting.report() } else ""
            delay(REPORT_MS)
        }
    }
    Column(Modifier.padding(16.dp)) {
        SelectionContainer {
            Text(
                report.ifEmpty { stringResource(R.string.perf_off) },
                style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace,
            )
        }
    }
}

/** The next Testing section starts open, to show the report after a scroll test. */
object TestingOpen {
    var next = false
}

private const val REPORT_MS = 2000L
