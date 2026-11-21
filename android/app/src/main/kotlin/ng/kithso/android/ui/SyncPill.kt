package ng.kithso.android.ui

import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.SizeTransform
import androidx.compose.animation.animateContentSize
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.VerticalDivider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.sync.SyncProgress
import ng.kithso.android.sync.SyncStatus
import ng.kithso.android.sync.Syncer
import ng.kithso.core.SyncOutlook

/** What the sync pill shows, worked out from [SyncStatus] alone. */
@Immutable
sealed interface PillState {
    /**
     * No session running: a summary of the next sync and Sync now. [needsChoice]: this device's
     * user hasn't chosen what it syncs, so it fetches nothing yet. [outlook] is null outside a
     * group or while sync is off ([running]). [finished]: the last session ended with tracks still
     * missing ("Finished, but 3 tracks are still missing"), shown in the summary's place with
     * Details; "" otherwise.
     */
    data class Idle(
        val outlook: SyncOutlook?,
        val needsChoice: Boolean,
        val running: Boolean,
        val canSync: Boolean,
        val finished: String = "",
    ) : PillState

    /**
     * Sessions running: how far the files have come ([fraction], null before any are counted),
     * what's left to receive and to send, whether this device transcodes for another, the speed
     * and time left ([pace], "" for none), and Cancel sync in Sync now's place.
     */
    data class Syncing(
        val fraction: Float?,
        val receiveLeft: Int,
        val sendLeft: Int,
        val transcoding: Boolean,
        val pace: String = "",
    ) : PillState
}

/** Progress only while a session runs: never otherwise, whatever else is going on. */
fun pillState(status: SyncStatus): PillState {
    val sessions = status.sessions.values
    if (sessions.isEmpty()) {
        return PillState.Idle(
            outlook = status.outlook.takeIf { status.running },
            needsChoice = status.needsChoice,
            running = status.running,
            canSync = status.running && status.members.isNotEmpty(),
            finished = status.finished.takeIf { status.running && !status.needsChoice }.orEmpty(),
        )
    }
    val moving = sessions.sumOf { it.moving }
    return PillState.Syncing(
        fraction = if (moving > 0) sessions.sumOf { it.moved }.toFloat() / moving else null,
        receiveLeft = sessions.sumOf { (it.filesTotal - it.filesDone).coerceAtLeast(0) },
        sendLeft = sessions.sumOf { (it.sendsTotal - it.sendsDone).coerceAtLeast(0) },
        transcoding = status.transcoding,
        pace = pillPace(sessions),
    )
}

/**
 * The speed and time left to show for [sessions], such as "2.1 MB/s · about 4 min left", or ""
 * for none. The core smooths and rounds them per session (mc_sync_progress's speed and
 * eta_seconds, worded by mc_sync_pace_text), and leaves them out until they've settled, during
 * a stall, and while the size is unknown, so every platform shows the same. Of several sessions
 * at once, none: one's speed would be neither the other's nor the sum, as in the notification.
 */
fun pillPace(sessions: Collection<SyncProgress>): String = sessions.singleOrNull()?.pace.orEmpty()

/**
 * The split sync pill: on the left what the next sync would copy, or while syncing what's left,
 * the speed and time left and a bar of how far it has come; on the right, beyond a divider, Sync
 * now, or Cancel sync in its place while syncing. The divider and the right-hand button stay put
 * as the pill changes between the two: only what's in them fades from one to the other, and the
 * height follows the left side. It goes in a surface of its own: [DockSurface] on the phone,
 * the sidebar's on a tablet.
 */
@Composable
fun SyncPillContent(
    status: SyncStatus,
    onSyncNow: () -> Unit,
    onCancel: () -> Unit,
    modifier: Modifier = Modifier,
    onDetails: (() -> Unit)? = null,
) {
    val state = pillState(status)
    Row(
        modifier.fillMaxWidth().heightIn(min = PillHeight).animateContentSize(),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        AnimatedContent(
            targetState = state,
            contentKey = { it is PillState.Syncing },
            transitionSpec = {
                fadeIn(tween(220, delayMillis = 90)) togetherWith fadeOut(tween(90)) using SizeTransform(clip = false)
            },
            contentAlignment = Alignment.CenterStart,
            modifier = Modifier.weight(1f),
            label = "sync pill",
        ) { shown ->
            when (shown) {
                is PillState.Idle -> IdleSummary(shown, onDetails)
                is PillState.Syncing -> SyncingSummary(shown)
            }
        }
        VerticalDivider(Modifier.height(28.dp), color = MaterialTheme.colorScheme.outlineVariant)
        PillButton(state, onSyncNow, onCancel)
    }
}

/** [SyncPillContent] for [syncer]'s status: Sync now and Cancel with every member. */
@Composable
fun SyncPill(syncer: Syncer, modifier: Modifier = Modifier) {
    val status by syncer.status.collectAsState()
    val showIssues = rememberSyncIssues(syncer)
    SyncPillContent(status, onSyncNow = { syncer.syncNow() }, onCancel = { syncer.cancelSync() }, modifier, onDetails = showIssues)
}

@Composable
private fun IdleSummary(state: PillState.Idle, onDetails: (() -> Unit)?) {
    val summary = when {
        !state.running -> stringResource(R.string.sync_is_off)
        state.needsChoice -> stringResource(R.string.sync_pill_choose)
        state.finished.isNotEmpty() -> state.finished
        else -> state.outlook?.text.orEmpty()
    }
    // Why tracks are still missing, from the problems.
    val details = onDetails?.takeIf { state.finished.isNotEmpty() && state.running && !state.needsChoice }
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Text(
            summary,
            style = MaterialTheme.typography.titleSmall,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f).padding(start = 20.dp, end = 12.dp).semantics { liveRegion = LiveRegionMode.Polite },
        )
        if (details != null) {
            TextButton(onClick = details) { Text(stringResource(R.string.sync_issues_details)) }
        }
    }
}

@Composable
private fun SyncingSummary(state: PillState.Syncing) {
    val parts = listOfNotNull(
        state.receiveLeft.takeIf { it > 0 }?.let { pluralStringResource(R.plurals.sync_pill_receive_left, it, it) },
        state.sendLeft.takeIf { it > 0 }?.let { pluralStringResource(R.plurals.sync_pill_send_left, it, it) },
        if (state.transcoding) stringResource(R.string.sync_pill_transcoding) else null,
    )
    val text = parts.ifEmpty { listOf(stringResource(R.string.syncing)) }.joinToString(stringResource(R.string.separator_dot))
    // The bar ends short of the divider, with the text: it never runs under Cancel sync.
    Column(Modifier.fillMaxWidth().padding(start = 20.dp, end = 12.dp, top = 10.dp, bottom = 10.dp)) {
        Text(
            text,
            style = MaterialTheme.typography.titleSmall,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
        )
        if (state.pace.isNotEmpty()) {
            Text(
                state.pace,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        val fraction = state.fraction
        val bar = Modifier.fillMaxWidth().padding(top = 8.dp)
        if (fraction != null) {
            LinearProgressIndicator(progress = { fraction }, modifier = bar)
        } else {
            LinearProgressIndicator(bar)
        }
    }
}

/**
 * Sync now, or Cancel sync while syncing, in one place: both are laid out on top of each other,
 * so the slot is as wide as the wider and doesn't move, and the one not shown fades out and
 * can't be pressed or focused.
 */
@Composable
private fun PillButton(state: PillState, onSyncNow: () -> Unit, onCancel: () -> Unit) {
    val syncing = state is PillState.Syncing
    val cancelAlpha by animateFloatAsState(if (syncing) 1f else 0f, tween(220), label = "cancel")
    Box(Modifier.padding(horizontal = 8.dp), contentAlignment = Alignment.Center) {
        val idle = state as? PillState.Idle
        TextButton(
            onClick = onSyncNow,
            enabled = idle?.canSync == true,
            modifier = Modifier.alpha(1f - cancelAlpha).then(if (syncing) Modifier.clearAndSetSemantics {} else Modifier),
        ) {
            Icon(Icons.Sync, contentDescription = null, modifier = Modifier.size(18.dp))
            Text(stringResource(R.string.sync_now), modifier = Modifier.padding(start = 8.dp))
        }
        TextButton(
            onClick = onCancel,
            enabled = syncing,
            modifier = Modifier.alpha(cancelAlpha).then(if (syncing) Modifier else Modifier.clearAndSetSemantics {}),
        ) {
            Text(stringResource(R.string.cancel_sync))
        }
    }
}

private val PillHeight = 64.dp
