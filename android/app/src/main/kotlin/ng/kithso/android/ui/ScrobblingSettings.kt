package ng.kithso.android.ui

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.util.Log
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
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
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import ng.kithso.android.R
import ng.kithso.android.playback.Scrobbling
import ng.kithso.core.CoreScrobbler

/**
 * Settings' Scrobbling section: the group's account on Last.fm or the service chosen instead
 * (sign in, in the browser, or sign out), what's waiting to be sent, and this phone's own
 * switch. Read again every few seconds while shown, since an account arriving by sync or
 * plays being sent change it.
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
    var choosingService by rememberSaveable { mutableStateOf(false) }

    val service = state.serviceName.ifEmpty { stringResource(R.string.scrobble_service) }
    val account = when {
        state.signingIn -> stringResource(R.string.scrobble_waiting, service)
        state.username != null -> stringResource(R.string.scrobble_signed_in, state.username!!)
        !state.hasKey -> stringResource(R.string.scrobble_no_key)
        else -> stringResource(R.string.scrobble_signed_out)
    }
    val detail = when {
        state.signingIn -> null
        state.timedOut -> stringResource(R.string.scrobble_sign_in_timed_out)
        state.failure != null -> stringResource(R.string.scrobble_sign_in_failed, statusText(context, state.failure!!))
        state.serviceFailure != null -> stringResource(R.string.scrobble_service_failed, statusText(context, state.serviceFailure!!))
        state.username == null -> null
        state.lastError == CoreScrobbler.STATUS_AUTH -> stringResource(R.string.scrobble_refused, service)
        state.lastError == CoreScrobbler.STATUS_NET -> stringResource(R.string.scrobble_offline, service)
        state.pending > 0 -> pluralStringResource(R.plurals.scrobble_pending, state.pending, state.pending)
        else -> null
    }
    val canSignIn = state.hasKey || state.username != null
    ListItem(
        headlineContent = { Text(service) },
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
        headlineContent = { Text(stringResource(R.string.scrobble_change_service)) },
        modifier = Modifier.clickable { choosingService = true },
    )
    ListItem(
        headlineContent = { Text(stringResource(R.string.scrobble_switch)) },
        trailingContent = { Switch(checked = state.enabled, onCheckedChange = null) },
        modifier = Modifier.toggleable(state.enabled, role = Role.Switch) { scrobbling.setEnabled(it) },
    )
    Text(
        stringResource(R.string.scrobble_note, service),
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp),
    )

    if (confirmSignOut) {
        AlertDialog(
            onDismissRequest = { confirmSignOut = false },
            title = { Text(stringResource(R.string.scrobble_sign_out_title, service)) },
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
    if (choosingService) {
        ServiceDialog(state, onDismiss = { choosingService = false }) { kind, apiUrl, authUrl, apiKey, apiSecret ->
            choosingService = false
            scrobbling.setService(kind, apiUrl, authUrl, apiKey, apiSecret)
        }
    }
}

/**
 * The Scrobbling service dialog: Last.fm, Libre.fm, or another service with Last.fm's API,
 * entered by hand. Starts from the one in use; another's addresses are filled in again, its
 * key and secret are typed afresh. OK is enabled once another's four fields are filled in,
 * with addresses starting http:// or https://.
 */
@Composable
private fun ServiceDialog(
    state: Scrobbling.State,
    onDismiss: () -> Unit,
    onChoose: (service: Int, apiUrl: String?, authUrl: String?, apiKey: String?, apiSecret: String?) -> Unit,
) {
    var service by rememberSaveable { mutableIntStateOf(state.service) }
    val other = state.service == CoreScrobbler.SERVICE_OTHER
    var apiUrl by rememberSaveable { mutableStateOf(if (other) state.apiUrl else "") }
    var authUrl by rememberSaveable { mutableStateOf(if (other) state.authUrl else "") }
    var apiKey by rememberSaveable { mutableStateOf("") }
    var apiSecret by remember { mutableStateOf("") } // not saved with the screen's state
    val fields = listOf(apiUrl, authUrl, apiKey, apiSecret).map { it.trim() }
    val valid = service != CoreScrobbler.SERVICE_OTHER ||
        (fields.all { it.isNotEmpty() } && isWebAddress(fields[0]) && isWebAddress(fields[1]))
    val choose = {
        if (valid) {
            if (service == CoreScrobbler.SERVICE_OTHER) onChoose(service, fields[0], fields[1], fields[2], fields[3])
            else onChoose(service, null, null, null, null)
        }
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.scrobble_service_title)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(stringResource(R.string.scrobble_service_text))
                Column(Modifier.selectableGroup()) {
                    listOf(
                        CoreScrobbler.SERVICE_LASTFM to stringResource(R.string.scrobble_service),
                        CoreScrobbler.SERVICE_LIBREFM to stringResource(R.string.scrobble_service_librefm),
                        CoreScrobbler.SERVICE_OTHER to stringResource(R.string.scrobble_service_other),
                    ).forEach { (kind, name) ->
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.fillMaxWidth()
                                .selectable(service == kind, role = Role.RadioButton) { service = kind }
                                .padding(vertical = 4.dp),
                        ) {
                            RadioButton(selected = service == kind, onClick = null)
                            Text(name, Modifier.padding(start = 12.dp))
                        }
                    }
                }
                if (service == CoreScrobbler.SERVICE_OTHER) {
                    ServiceField(apiUrl, { apiUrl = it }, R.string.scrobble_service_api_url, KeyboardType.Uri)
                    ServiceField(authUrl, { authUrl = it }, R.string.scrobble_service_auth_url, KeyboardType.Uri)
                    ServiceField(apiKey, { apiKey = it }, R.string.scrobble_service_api_key, KeyboardType.Ascii)
                    ServiceField(apiSecret, { apiSecret = it }, R.string.scrobble_service_api_secret, KeyboardType.Password)
                    if (!valid) {
                        Text(
                            stringResource(R.string.scrobble_service_incomplete),
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
        },
        confirmButton = { TextButton(onClick = choose, enabled = valid) { Text(stringResource(R.string.ok)) } },
        dismissButton = { TextButton(onClick = onDismiss) { Text(stringResource(R.string.cancel)) } },
    )
}

@Composable
private fun ServiceField(value: String, onValueChange: (String) -> Unit, label: Int, type: KeyboardType) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        label = { Text(stringResource(label)) },
        singleLine = true,
        visualTransformation = if (type == KeyboardType.Password) PasswordVisualTransformation() else VisualTransformation.None,
        keyboardOptions = KeyboardOptions(keyboardType = type, imeAction = ImeAction.Next),
        modifier = Modifier.fillMaxWidth(),
    )
}

private fun isWebAddress(s: String): Boolean = listOf("http://", "https://").any {
    s.length > it.length && s.startsWith(it, ignoreCase = true)
}

/** Opens the service's page to approve the sign-in in the browser. */
private fun Context.openPage(url: String) {
    try {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    } catch (e: ActivityNotFoundException) {
        Log.w("kithsong", "No browser to sign in with", e)
    }
}

private const val REFRESH_MS = 2000L
