package ng.kithso.android.ui

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import ng.kithso.android.R
import ng.kithso.android.playback.Scrobbling
import ng.kithso.core.CoreScrobbler

/**
 * Settings' Scrobbling section: the group's Last.fm account (sign in, in the browser, or sign
 * out), what's waiting to be sent, and this phone's own switch. Read again every few seconds
 * while shown, since an account arriving by sync or plays being sent change it.
 */
@Composable
fun ScrobblingSettings(scrobbling: Scrobbling) {
    val context = LocalContext.current
    val state by scrobbling.state.collectAsState()
    LaunchedEffect(scrobbling) {
        while (true) {
            scrobbling.refresh()
            delay(REFRESH_MS)
        }
    }
    var confirmSignOut by rememberSaveable { mutableStateOf(false) }

    val account = when {
        state.signingIn -> stringResource(R.string.scrobble_waiting)
        state.username != null -> stringResource(R.string.scrobble_signed_in, state.username!!)
        !state.hasKey -> stringResource(R.string.scrobble_no_key)
        else -> stringResource(R.string.scrobble_signed_out)
    }
    val detail = when {
        state.signingIn -> null
        state.timedOut -> stringResource(R.string.scrobble_sign_in_timed_out)
        state.failure != null -> stringResource(R.string.scrobble_sign_in_failed, statusText(context, state.failure!!))
        state.username == null -> null
        state.lastError == CoreScrobbler.STATUS_AUTH -> stringResource(R.string.scrobble_refused)
        state.lastError == CoreScrobbler.STATUS_NET -> stringResource(R.string.scrobble_offline)
        state.pending > 0 -> pluralStringResource(R.plurals.scrobble_pending, state.pending, state.pending)
        else -> null
    }
    val canSignIn = state.hasKey || state.username != null
    ListItem(
        headlineContent = { Text(stringResource(R.string.scrobble_service)) },
        supportingContent = {
            Column(Modifier.semantics { liveRegion = LiveRegionMode.Polite }) {
                Text(account)
                detail?.let { Text(it, color = MaterialTheme.colorScheme.error) }
            }
        },
        trailingContent = {
            when {
                state.signingIn -> TextButton(onClick = scrobbling::cancelSignIn) { Text(stringResource(R.string.cancel)) }
                state.username != null -> TextButton(onClick = { confirmSignOut = true }) {
                    Text(stringResource(R.string.scrobble_sign_out))
                }
                else -> TextButton(onClick = { scrobbling.signIn(context::openPage) }, enabled = canSignIn) {
                    Text(stringResource(R.string.scrobble_sign_in_short))
                }
            }
        },
    )
    ListItem(
        headlineContent = { Text(stringResource(R.string.scrobble_switch)) },
        trailingContent = { Switch(checked = state.enabled, onCheckedChange = null) },
        modifier = Modifier.toggleable(state.enabled, role = Role.Switch) { scrobbling.setEnabled(it) },
    )
    Text(
        stringResource(R.string.scrobble_note),
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
    )

    if (confirmSignOut) {
        AlertDialog(
            onDismissRequest = { confirmSignOut = false },
            title = { Text(stringResource(R.string.scrobble_sign_out_title)) },
            text = { Text(stringResource(R.string.scrobble_sign_out_text)) },
            confirmButton = {
                TextButton(onClick = {
                    confirmSignOut = false
                    scrobbling.signOut()
                }) { Text(stringResource(R.string.scrobble_sign_out)) }
            },
            dismissButton = { TextButton(onClick = { confirmSignOut = false }) { Text(stringResource(R.string.cancel)) } },
        )
    }
}

/** Opens Last.fm's page to approve the sign-in in the browser. */
private fun Context.openPage(url: String) {
    try {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    } catch (e: ActivityNotFoundException) {
        Log.w("kithsong", "No browser to sign in with", e)
    }
}

private const val REFRESH_MS = 2000L
