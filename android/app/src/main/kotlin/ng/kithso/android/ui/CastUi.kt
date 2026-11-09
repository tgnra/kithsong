package ng.kithso.android.ui

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import ng.kithso.android.R
import ng.kithso.android.playback.CastControl
import ng.kithso.android.playback.CastDevice

/**
 * The full player's cast button, for [cast] where it's [CastControl.available]: the cast icon,
 * the connected one while casting, and that one pulsing while connecting. It opens
 * [CastPicker], to pick a device or stop casting. Its name for accessibility says which.
 */
@Composable
internal fun CastButton(cast: CastControl, modifier: Modifier = Modifier) {
    var picking by rememberSaveable { mutableStateOf(false) }
    val name = castDeviceName(cast)
    val description = when {
        cast.casting -> stringResource(R.string.cast_casting_to, name)
        cast.connecting -> stringResource(R.string.cast_connecting_to, name)
        else -> stringResource(R.string.cast)
    }
    IconButton(onClick = { picking = true }, modifier = modifier) {
        when {
            cast.casting -> Icon(Icons.CastConnected, contentDescription = description)
            cast.connecting -> {
                val pulse = rememberInfiniteTransition(label = "cast connecting")
                // Read in the layer, so the pulse redraws the icon without recomposing it.
                val alpha = pulse.animateFloat(
                    initialValue = 1f,
                    targetValue = 0.3f,
                    animationSpec = infiniteRepeatable(tween(CONNECTING_PULSE_MS), RepeatMode.Reverse),
                    label = "cast connecting alpha",
                )
                Icon(
                    Icons.CastConnected,
                    contentDescription = description,
                    modifier = Modifier.graphicsLayer { this.alpha = alpha.value },
                )
            }
            else -> Icon(Icons.Cast, contentDescription = description)
        }
    }
    if (picking) CastPicker(cast, onDismiss = { picking = false })
}

/** The connected or connecting device's name, or a stand-in for one without. */
@Composable
private fun castDeviceName(cast: CastControl) =
    cast.deviceName?.takeIf { it.isNotBlank() } ?: stringResource(R.string.cast_device_unnamed)

/**
 * A bottom sheet for [CastButton]. Not casting, it looks for devices while it's open and lists
 * them as they're found; picking one slides the sheet away, then connects to it. Casting, or
 * connecting, it names the device, has the volume once connected (the player may not have room
 * for it), and Stop casting.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CastPicker(cast: CastControl, onDismiss: () -> Unit) {
    val state = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val scope = rememberCoroutineScope()
    val close: (() -> Unit) -> Unit = { then ->
        scope.launch { state.hide() }.invokeOnCompletion {
            onDismiss()
            then()
        }
    }
    // Connecting or connected: nothing to look for.
    val session = cast.connecting || cast.casting
    DisposableEffect(cast, session) {
        if (!session) cast.startDiscovery()
        onDispose { if (!session) cast.stopDiscovery() }
    }
    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = state) {
        Column(Modifier.navigationBarsPadding().padding(bottom = 8.dp)) {
            if (session) {
                val name = castDeviceName(cast)
                PickerHeading(
                    stringResource(if (cast.casting) R.string.cast_casting_to else R.string.cast_connecting_to, name),
                    busy = !cast.casting,
                )
                if (cast.casting) {
                    CastVolumeSlider(
                        cast,
                        Modifier.fillMaxWidth().padding(horizontal = PickerPadding).padding(bottom = 8.dp),
                    )
                }
                ListItem(
                    headlineContent = { Text(stringResource(R.string.cast_stop)) },
                    leadingContent = { Icon(Icons.Clear, contentDescription = null) },
                    colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
                    modifier = Modifier.clickable { close(cast::disconnect) },
                )
            } else {
                val devices = cast.devices
                // It keeps looking while open, so the heading's spinner stays.
                PickerHeading(stringResource(R.string.cast_to), busy = true)
                if (devices.isEmpty()) {
                    Column(
                        Modifier.fillMaxWidth().padding(horizontal = PickerPadding, vertical = 16.dp),
                        verticalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        Text(stringResource(R.string.cast_looking), style = MaterialTheme.typography.bodyLarge)
                        Text(
                            stringResource(R.string.cast_looking_text),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                } else {
                    val connectLabel = stringResource(R.string.connect)
                    LazyColumn {
                        items(devices, key = { it.id }) { device ->
                            DeviceRow(device, connectLabel) { close { cast.connect(device) } }
                        }
                    }
                }
            }
        }
    }
}

/** [text] at the top of [CastPicker], with a small spinner after it while [busy]. */
@Composable
private fun PickerHeading(text: String, busy: Boolean) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = PickerPadding).padding(bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text,
            style = MaterialTheme.typography.titleMedium,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.weight(1f).semantics { heading() },
        )
        // The heading and the list say what's going on; the spinner is only to see.
        if (busy) CircularProgressIndicator(Modifier.size(18.dp).clearAndSetSemantics {}, strokeWidth = 2.dp)
    }
}

@Composable
private fun DeviceRow(device: CastDevice, connectLabel: String, onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(device.name, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = if (!device.description.isNullOrBlank()) ({
            Text(device.description, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }) else null,
        leadingContent = { Icon(Icons.Cast, contentDescription = null) },
        colors = ListItemDefaults.colors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
        modifier = Modifier.clickable(onClickLabel = connectLabel, onClick = onClick),
    )
}

/**
 * Under the full player's controls while casting: "Casting to" the device, over a short volume
 * slider for it ([CastVolumeSlider]), [CastVolumeWidth] at the most.
 */
@Composable
internal fun CastVolume(cast: CastControl, modifier: Modifier = Modifier) {
    Column(modifier.widthIn(max = CastVolumeWidth).fillMaxWidth(), horizontalAlignment = Alignment.CenterHorizontally) {
        Text(
            stringResource(R.string.cast_casting_to, castDeviceName(cast)),
            style = MaterialTheme.typography.labelMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
        )
        CastVolumeSlider(cast, Modifier.fillMaxWidth())
    }
}

/**
 * The cast device's volume, between quiet and loud speaker icons, slim as the mini player's
 * seekbar. The thumb follows the finger, and the device follows the thumb as it moves, at most
 * every [VOLUME_SEND_MS]; let go, the thumb stays where it was left for [VOLUME_SETTLE_MS], for
 * the device to catch up, then follows the device again, as when its own buttons change it.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun CastVolumeSlider(cast: CastControl, modifier: Modifier = Modifier) {
    // The drag's volume, while dragging and for a moment after.
    var held by remember { mutableStateOf<Float?>(null) }
    var dragging by remember { mutableStateOf(false) }
    var lastSent by remember { mutableLongStateOf(0L) }
    LaunchedEffect(dragging, held != null) {
        if (!dragging && held != null) {
            delay(VOLUME_SETTLE_MS)
            held = null
        }
    }
    val interaction = remember { MutableInteractionSource() }
    val description = stringResource(R.string.volume)
    val iconModifier = Modifier.size(20.dp)
    val iconColor = MaterialTheme.colorScheme.onSurfaceVariant
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Icon(Icons.VolumeDown, contentDescription = null, tint = iconColor, modifier = iconModifier)
        Slider(
            value = held ?: cast.volume.coerceIn(0f, 1f),
            onValueChange = { value ->
                dragging = true
                held = value
                val now = System.currentTimeMillis()
                if (now - lastSent >= VOLUME_SEND_MS) {
                    lastSent = now
                    cast.setVolume(value)
                }
            },
            onValueChangeFinished = {
                held?.let(cast::setVolume)
                lastSent = 0L
                dragging = false
            },
            interactionSource = interaction,
            thumb = { SliderDefaults.Thumb(interaction, thumbSize = DpSize(4.dp, 16.dp)) },
            track = { state ->
                SliderDefaults.Track(
                    state,
                    modifier = Modifier.height(4.dp),
                    drawStopIndicator = null,
                    thumbTrackGapSize = 2.dp,
                )
            },
            modifier = Modifier
                .weight(1f)
                .padding(horizontal = 8.dp)
                .height(28.dp)
                .semantics { contentDescription = description },
        )
        Icon(Icons.VolumeUp, contentDescription = null, tint = iconColor, modifier = iconModifier)
    }
}

/** Either side of [CastPicker]'s heading and text, as ListItem has its content. */
private val PickerPadding = 16.dp

/** The most width of [CastVolume]: a short slider, as volume needs no precision. */
private val CastVolumeWidth = 280.dp

private const val CONNECTING_PULSE_MS = 700
private const val VOLUME_SEND_MS = 100L
private const val VOLUME_SETTLE_MS = 1000L
