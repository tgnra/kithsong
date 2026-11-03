package ng.kithso.wear.playback

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue

/**
 * Where the watch's sound goes. Music belongs in Bluetooth headphones rather than the watch's
 * speaker, which many watches haven't got: before playing, the UI asks for headphones unless
 * some are connected ([bluetoothConnected], Compose state following the outputs as they come
 * and go) or the user chose the speaker for this run of the app ([useSpeaker]). Nothing here
 * routes the sound: Android sends it to the headphones itself once they're connected. Use on
 * the main thread.
 */
class AudioOutputs(private val context: Context) {
    private val audio = context.getSystemService(AudioManager::class.java)

    var bluetoothConnected by mutableStateOf(false)
        private set

    /** Play through the watch's speaker: chosen once, not asked again every track. */
    var useSpeaker by mutableStateOf(false)

    /** Whether playing now would be heard where the user wants it. */
    val ready: Boolean get() = bluetoothConnected || useSpeaker

    val hasSpeaker: Boolean get() = outputs().any { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }

    init {
        refresh()
        audio.registerAudioDeviceCallback(
            object : AudioDeviceCallback() {
                override fun onAudioDevicesAdded(addedDevices: Array<AudioDeviceInfo>) = refresh()
                override fun onAudioDevicesRemoved(removedDevices: Array<AudioDeviceInfo>) = refresh()
            },
            Handler(Looper.getMainLooper()),
        )
    }

    private fun refresh() {
        bluetoothConnected = outputs().any { it.type in BLUETOOTH_TYPES }
    }

    private fun outputs(): Array<AudioDeviceInfo> = audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS)

    /** Opens the system's Bluetooth settings, where headphones are paired and connected. */
    fun openBluetoothSettings() {
        try {
            context.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        } catch (e: ActivityNotFoundException) {
            Log.w(TAG, "No Bluetooth settings to open", e)
        }
    }

    private companion object {
        const val TAG = "kithsong"

        // Headphones, earbuds, speakers and hearing aids over Bluetooth: classic audio, and
        // from API 31 LE Audio.
        val BLUETOOTH_TYPES: Set<Int> = buildSet {
            add(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP)
            add(AudioDeviceInfo.TYPE_HEARING_AID)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                add(AudioDeviceInfo.TYPE_BLE_HEADSET)
                add(AudioDeviceInfo.TYPE_BLE_SPEAKER)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) add(AudioDeviceInfo.TYPE_BLE_BROADCAST)
        }
    }
}
