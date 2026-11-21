package ng.kithso.android.sync

import android.content.Context
import android.net.ConnectivityManager
import android.net.LinkProperties
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.util.Log
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * Runs [action] once [delayMs] after the last [poke], so a burst of pokes runs it once. Touch it
 * from one thread at a time ([dispatcher]'s, where [action] runs too).
 */
class Debouncer(
    private val scope: CoroutineScope,
    private val dispatcher: CoroutineDispatcher,
    private val delayMs: Long,
    private val action: () -> Unit,
) {
    private var job: Job? = null

    fun poke() {
        job?.cancel()
        job = scope.launch(dispatcher) {
            delay(delayMs)
            job = null
            action()
        }
    }

    fun cancel() {
        job?.cancel()
        job = null
    }
}

/**
 * Whether what a network monitor saw is a change sync should hear of: the LAN address moved
 * ([key], e.g. another Wi-Fi network), or a network was lost meanwhile, so the sockets may be
 * dead even if it came back with the same address. The callbacks Android sends for the networks
 * already there on registering change nothing, so they're skipped.
 */
class NetworkChanges(private var key: String?) {
    private var lost = false

    fun lost() {
        lost = true
    }

    /** True if sync should be told, given the address now. */
    fun settle(now: String?): Boolean {
        val changed = lost || now != key
        key = now
        lost = false
        return changed
    }
}

/**
 * Watches Wi-Fi and Ethernet networks while sync runs and calls [onChanged] on [dispatcher], a
 * second after they settle, when [NetworkChanges] says sync should hear of it. [address] reads
 * this phone's LAN address now.
 */
internal class NetworkWatch(
    context: Context,
    private val scope: CoroutineScope,
    private val dispatcher: CoroutineDispatcher,
    private val address: () -> String?,
    private val onChanged: () -> Unit,
) {
    private val connectivity = context.getSystemService(ConnectivityManager::class.java)
    private var changes = NetworkChanges(null)
    private val debouncer = Debouncer(scope, dispatcher, DEBOUNCE_MS) {
        if (changes.settle(address())) onChanged()
    }
    private var registered = false

    private val callback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) = poke()

        override fun onLost(network: Network) {
            scope.launch(dispatcher) {
                changes.lost()
                debouncer.poke()
            }
        }

        override fun onLinkPropertiesChanged(network: Network, linkProperties: LinkProperties) = poke()
    }

    private fun poke() {
        scope.launch(dispatcher) { debouncer.poke() }
    }

    /** On [dispatcher]. */
    fun start() {
        if (registered || connectivity == null) return
        changes = NetworkChanges(address())
        val request = NetworkRequest.Builder()
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .addTransportType(NetworkCapabilities.TRANSPORT_ETHERNET)
            .build()
        try {
            connectivity.registerNetworkCallback(request, callback)
            registered = true
        } catch (e: RuntimeException) {
            // Too many callbacks registered app-wide, or no permission: sync works without.
            Log.w(TAG, "Couldn't watch the network", e)
        }
    }

    /** On [dispatcher]. */
    fun stop() {
        debouncer.cancel()
        if (!registered) return
        registered = false
        try {
            connectivity?.unregisterNetworkCallback(callback)
        } catch (e: IllegalArgumentException) {
            Log.w(TAG, "Network callback wasn't registered", e)
        }
    }

    private companion object {
        const val TAG = "kithsong"
        const val DEBOUNCE_MS = 1_000L
    }
}
