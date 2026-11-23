package ng.kithso.android.cast

import android.content.Context
import android.net.ConnectivityManager
import java.io.BufferedOutputStream
import java.io.Closeable
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.security.MessageDigest
import java.security.SecureRandom

/** What the Cast device plays: [url], a live stream of [contentType], until it's closed. */
interface CastStream : AutoCloseable {
    val url: String
    val contentType: String

    override fun close()
}

/**
 * A small HTTP server on the phone's address on the network the Cast device is on. Everything it
 * serves is under a path no one else there can guess ([secret]), as it's the user's. A subclass
 * answers requests ([respond], each on a thread of its own), and calls [listen] once it's ready to.
 */
abstract class LocalServer(address: InetAddress) : AutoCloseable {
    private val socket = ServerSocket().apply {
        reuseAddress = true
        bind(InetSocketAddress(address, 0), BACKLOG)
    }

    /** 32 hex digits, new for each server. */
    protected val secret = ByteArray(16).also(SecureRandom()::nextBytes).joinToString("") { "%02x".format(it) }
    protected val base = "http://${hostOf(address)}:${socket.localPort}"
    protected val lock = Any()
    @Volatile protected var closed = false
        private set

    // Every connection open, under lock.
    private val clients = HashSet<Socket>()

    protected fun listen() = thread("kithsong-cast-accept") { accept() }

    /** Stops listening and drops every connection. */
    override fun close() {
        val open = synchronized(lock) {
            closed = true
            clients.toList()
        }
        closeQuietly(socket)
        open.forEach(::closeQuietly)
    }

    /**
     * Answers a request for [path] (its query left out), all of it, or all it wants to: the
     * connection closes after. Throws IOException when the device lets go of it.
     */
    protected abstract fun respond(client: Socket, method: String, path: String, out: OutputStream)

    private fun accept() {
        while (!closed) {
            val client = try {
                socket.accept()
            } catch (e: IOException) {
                break
            }
            synchronized(lock) {
                if (closed) closeQuietly(client) else clients += client
            }
            if (!closed) thread("kithsong-cast-stream") { serve(client) }
        }
    }

    private fun serve(client: Socket) {
        try {
            client.soTimeout = REQUEST_TIMEOUT_MS
            val request = readRequest(client.getInputStream())?.split(' ') ?: return
            val out = BufferedOutputStream(client.getOutputStream(), 64 * 1024)
            respond(client, request[0].uppercase(), request.getOrNull(1)?.substringBefore('?').orEmpty(), out)
            out.flush()
        } catch (e: IOException) {
            // The device let go of it, a newer request took over, or the session ended.
        } finally {
            synchronized(lock) { clients -= client }
            closeQuietly(client)
        }
    }

    companion object {
        private const val BACKLOG = 8
        private const val REQUEST_TIMEOUT_MS = 10_000
        private const val MAX_REQUEST_BYTES = 16 * 1024

        /**
         * The phone's address towards [device] (the Cast device's), or failing that the active
         * network's IPv4 address; null if there's neither. Not on the main thread.
         */
        fun localAddress(context: Context, device: InetAddress?): InetAddress? =
            device?.let(::addressTowards) ?: activeNetworkAddress(context)

        // The local address the system would send to [device] from: connecting a UDP socket
        // picks the route without sending anything.
        private fun addressTowards(device: InetAddress): InetAddress? = try {
            DatagramSocket().use { socket ->
                socket.connect(device, 8009) // the Cast port; any would do
                socket.localAddress?.takeIf { it is Inet4Address && !it.isAnyLocalAddress && !it.isLoopbackAddress }
            }
        } catch (e: SocketException) {
            null
        }

        private fun activeNetworkAddress(context: Context): InetAddress? {
            val connectivity = context.getSystemService(ConnectivityManager::class.java) ?: return null
            val network = connectivity.activeNetwork ?: return null
            val link = connectivity.getLinkProperties(network) ?: return null
            return link.linkAddresses.map { it.address }.firstOrNull { it is Inet4Address && !it.isLoopbackAddress }
        }

        private fun hostOf(address: InetAddress): String =
            address.hostAddress.orEmpty().let { if (address is Inet4Address) it else "[${it.substringBefore('%')}]" }

        // The request line, after reading up to the blank line that ends the headers; null for
        // a connection that sent none.
        private fun readRequest(input: InputStream): String? {
            val line = StringBuilder()
            var first: String? = null
            var total = 0
            while (true) {
                val c = input.read()
                if (c < 0) return null
                if (++total > MAX_REQUEST_BYTES) return null
                when (c) {
                    '\r'.code -> Unit
                    '\n'.code -> {
                        if (line.isEmpty()) return first
                        if (first == null) first = line.toString()
                        line.setLength(0)
                    }
                    else -> line.append(c.toChar())
                }
            }
        }

        /** Whether [asked] is [path], in a time that doesn't tell how much of it was right. */
        fun samePath(asked: String, path: String) = MessageDigest.isEqual(asked.toByteArray(), path.toByteArray())

        /** [headers] each end in CRLF. */
        fun writeHead(out: OutputStream, status: String, headers: String) {
            out.write("HTTP/1.1 $status\r\n${headers}Connection: close\r\n\r\n".toByteArray(Charsets.ISO_8859_1))
        }

        fun thread(name: String, block: () -> Unit) = Thread(block, name).apply { isDaemon = true }.start()

        fun closeQuietly(socket: Closeable) {
            try {
                socket.close()
            } catch (_: IOException) {
            }
        }
    }
}
