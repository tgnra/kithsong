package ng.kithso.android.ui

import android.widget.Toast
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import ng.kithso.android.R
import ng.kithso.android.sync.LostRemoval
import ng.kithso.android.sync.Syncer
import ng.kithso.core.SyncArchiveOffer
import ng.kithso.core.SyncIssue
import ng.kithso.core.SyncIssueKind
import ng.kithso.core.SyncProblems

/** What a problem in [SyncIssuesDialog] offers to do about it, beside saying how to fix it. */
sealed interface IssueAction {
    data object None : IssueAction

    /** Remove the tracks no device has any more, after asking ([SyncIssueKind.Lost]). */
    data object RemoveLost : IssueAction

    /** Make this device the archive in place of the lost one, [label] on the button. */
    data class TakeOver(val uid: String, val label: String) : IssueAction
}

/**
 * What [issue] offers: removing lost tracks while there are any, or taking over from a lost
 * archive while [offer] is for it and this device has room.
 */
fun issueAction(issue: SyncIssue, problems: SyncProblems, offer: SyncArchiveOffer?): IssueAction = when (issue.kind) {
    SyncIssueKind.Lost -> if (problems.lost.isNotEmpty()) IssueAction.RemoveLost else IssueAction.None
    SyncIssueKind.ArchiveLost, SyncIssueKind.ArchiveReinstall ->
        if (offer != null && offer.uid == issue.deviceUid && offer.canTake && offer.button.isNotEmpty()) {
            IssueAction.TakeOver(offer.uid, offer.button)
        } else {
            IssueAction.None
        }
    else -> IssueAction.None
}

/** The issues the dialog lists: those about [deviceUid], or all for null; any the core has no words for are left out. */
fun shownIssues(issues: List<SyncIssue>, deviceUid: String? = null): List<SyncIssue> =
    issues.filter { it.title.isNotBlank() && (deviceUid == null || it.deviceUid == deviceUid) }

/** The one-time dialog asks about [offer] now: there is one and it was never shown. */
fun asksOffer(offer: SyncArchiveOffer?): Boolean = offer != null && !offer.asked

/** The offer's title: the core words it for a phone, so a tablet has its own. */
@Composable
fun offerTitle(offer: SyncArchiveOffer, tablet: Boolean): String = when {
    tablet -> stringResource(R.string.sync_offer_title_tablet)
    offer.title.isNotEmpty() -> offer.title
    else -> stringResource(R.string.sync_offer_title_phone)
}

/**
 * Every problem with sync, or those about [deviceUid], each with what happened and how to fix
 * it, and a button where something can be done here: removing lost tracks, taking over from a
 * lost archive. [onDismiss] runs once it closes, which forgets the notices.
 */
@Composable
fun SyncIssuesDialog(
    problems: SyncProblems,
    offer: SyncArchiveOffer?,
    deviceUid: String?,
    onRemoveLost: suspend (List<String>) -> LostRemoval,
    onTakeOver: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    var removing by rememberSaveable { mutableStateOf(false) }
    val issues = shownIssues(problems.issues, deviceUid)
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.sync_issues_title)) },
        text = {
            Column(
                Modifier.verticalScroll(rememberScrollState()),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                if (issues.isEmpty()) Text(stringResource(R.string.sync_issues_none))
                for (issue in issues) {
                    IssueItem(issue, issueAction(issue, problems, offer), onRemoveLost = { removing = true }, onTakeOver)
                }
            }
        },
        confirmButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.close)) } },
    )
    if (removing && problems.lost.isNotEmpty()) {
        LostDialog(problems, onRemoveLost, onDismiss = { removing = false })
    }
}

@Composable
private fun IssueItem(issue: SyncIssue, action: IssueAction, onRemoveLost: () -> Unit, onTakeOver: (String) -> Unit) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        Text(issue.title, style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.Bold)
        if (issue.body.isNotEmpty()) Text(issue.body, style = MaterialTheme.typography.bodyMedium)
        if (issue.fix.isNotEmpty()) {
            Text(stringResource(R.string.sync_issue_fix, issue.fix), style = MaterialTheme.typography.bodyMedium)
        }
        when (action) {
            IssueAction.None -> Unit
            IssueAction.RemoveLost -> TextButton(onClick = onRemoveLost) { Text(stringResource(R.string.sync_lost_remove)) }
            is IssueAction.TakeOver -> TextButton(onClick = { onTakeOver(action.uid) }) { Text(action.label) }
        }
    }
}

/**
 * Hosts [SyncIssuesDialog] for [syncer]'s problems, about [deviceUid] or all for null, and returns
 * what opens it. Closing it forgets the notices.
 */
@Composable
fun rememberSyncIssues(syncer: Syncer, deviceUid: String? = null): () -> Unit {
    var open by rememberSaveable { mutableStateOf(false) }
    if (open) {
        val status by syncer.status.collectAsState()
        val takeOver = rememberTakeOver(syncer)
        SyncIssuesDialog(
            status.problems,
            status.offer,
            deviceUid,
            onRemoveLost = syncer::removeLost,
            onTakeOver = { uid ->
                open = false
                takeOver(uid)
            },
            onDismiss = {
                open = false
                syncer.dismissNotices()
            },
        )
    }
    return remember { { open = true } }
}

/** What starts taking over from a lost archive, saying so if the core refuses. */
@Composable
fun rememberTakeOver(syncer: Syncer): (String) -> Unit {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    return remember(syncer) {
        { uid ->
            scope.launch {
                if (!syncer.takeOverArchive(uid)) {
                    Toast.makeText(context, context.getString(R.string.sync_takeover_failed), Toast.LENGTH_LONG).show()
                }
            }
        }
    }
}

/**
 * The offer to take over from a lost archive, as a dialog once ever ([asksOffer]): marked seen
 * however it closes, after which [ArchiveOfferCard] carries it.
 */
@Composable
fun ArchiveOfferDialog(syncer: Syncer, tablet: Boolean) {
    val status by syncer.status.collectAsState()
    val offer = status.offer
    // Held here once shown, so the dialog doesn't vanish when the core marks it seen.
    var shown by remember { mutableStateOf<SyncArchiveOffer?>(null) }
    LaunchedEffect(offer?.uid, offer?.asked) {
        if (asksOffer(offer) && shown == null) {
            shown = offer
            syncer.archiveOfferSeen(offer!!.uid)
        }
    }
    val asking = shown ?: return
    val takeOver = rememberTakeOver(syncer)
    AlertDialog(
        onDismissRequest = { shown = null },
        title = { Text(offerTitle(asking, tablet)) },
        text = { Text(asking.body, Modifier.verticalScroll(rememberScrollState())) },
        confirmButton = {
            if (asking.canTake && asking.button.isNotEmpty()) {
                TextButton(onClick = {
                    shown = null
                    takeOver(asking.uid)
                }) { Text(asking.button) }
            } else {
                TextButton(onClick = { shown = null }) { Text(stringResource(R.string.close)) }
            }
        },
        dismissButton = if (asking.canTake && asking.button.isNotEmpty()) {
            { TextButton(onClick = { shown = null }) { Text(stringResource(R.string.sync_offer_later)) } }
        } else {
            null
        },
    )
}

/** The offer to take over from a lost archive once its dialog was shown: above the group on a phone, on this tablet's page. */
@Composable
fun ArchiveOfferCard(offer: SyncArchiveOffer, tablet: Boolean, onTakeOver: (String) -> Unit, modifier: Modifier = Modifier) {
    if (!offer.asked) return
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.errorContainer,
            contentColor = MaterialTheme.colorScheme.onErrorContainer,
        ),
        modifier = modifier.fillMaxWidth(),
    ) {
        Column(Modifier.padding(start = 16.dp, end = 16.dp, top = 12.dp, bottom = if (offer.canTake) 4.dp else 12.dp)) {
            Text(offerTitle(offer, tablet), style = MaterialTheme.typography.titleMedium)
            if (offer.body.isNotEmpty()) {
                Text(offer.body, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.padding(top = 4.dp))
            }
            if (offer.canTake && offer.button.isNotEmpty()) {
                TextButton(onClick = { onTakeOver(offer.uid) }, modifier = Modifier.align(Alignment.End)) {
                    Text(offer.button, color = MaterialTheme.colorScheme.onErrorContainer)
                }
            }
        }
    }
}
