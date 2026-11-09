package ng.kithso.android.cast

import android.content.ContentResolver
import android.content.Context
import android.net.ConnectivityManager
import android.net.Uri
import android.util.Log
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.io.RandomAccessFile
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketException
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Collections
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * Serves the music to a Cast device, which fetches what it plays by URL, while the files are in
 * the app's own storage. It runs only while a cast session is connected: [start] it then, and
 * [close] it as the session ends.
 *
 * It listens on a port the system picks, on the phone's address on the network the device is
 * on. Every URL starts with a token, random for each session, and serves only what [add] has
 * put up (the tracks CastPlayer has given the device, and their art), so nothing else in app
 * storage can be fetched, by the device or anyone else on the network. Tracks are served as
 * they are, with their type and length, and with byte ranges (206), which the device uses to
 * seek. Each connection is answered on a thread of its own, a request at a time, and closed.
 */
class CastServer private constructor(
    private val socket: ServerSocket,
    private val resolver: ContentResolver,
) {
    /** What a URL serves: a file as it is, or art read through ArtProvider's content URI. */
    sealed interface Served {
        data class Track(val file: File) : Served
        data class Art(val uri: Uri) : Served
    }

    private val token = ByteArray(16).also(SecureRandom()::nextBytes).joinToString("") { "%02x".format(it) }
    private val served = ConcurrentHashMap<String, Served>()
    private val clients: MutableSet<Socket> = Collections.newSetFromMap(ConcurrentHashMap())
    private val pool: ExecutorService = Executors.newFixedThreadPool(THREADS) { Thread(it, "kithsong-cast-http").apply { isDaemon = true } }
    @Volatile private var closed = false

    private val base = "http://${socket.inetAddress.hostAddress}:${socket.localPort}/$token/"

    init {
        Thread({ acceptLoop() }, "kithsong-cast-accept").apply { isDaemon = true }.start()
    }

    /** Puts [what] up as [name] (letters, digits and dots), and gives the URL it's at. */
    fun add(name: String, what: Served): String {
        served[name] = what
        return base + name
    }

    /** Takes down all but [names]. */
    fun keepOnly(names: Set<String>) {
        served.keys.retainAll(names)
    }

    /** The name in a URL of [add]'s, or null if it isn't one. */
    fun nameOf(url: String?): String? = url?.takeIf { it.startsWith(base) }?.removePrefix(base)

    /** Stops listening and drops every connection. */
    fun close() {
        closed = true
        try {
            socket.close()
        } catch (_: IOException) {
        }
        clients.toList().forEach { closeQuietly(it) }
        pool.shutdownNow()
        served.clear()
    }

    private fun acceptLoop() {
        while (!closed) {
            val client = try {
                socket.accept()
            } catch (e: IOException) {
                if (!closed) Log.w(TAG, "Cast server stopped accepting", e)
                break
            }
            clients += client
            try {
                pool.execute { serve(client) }
            } catch (e: Exception) { // shut down meanwhile
                clients -= client
                closeQuietly(client)
            }
        }
    }

    private fun serve(client: Socket) {
        try {
            client.soTimeout = READ_TIMEOUT_MS
            val input = BufferedInputStream(client.getInputStream())
            val output = BufferedOutputStream(client.getOutputStream())
            val request = readRequest(input) ?: return
            respond(request, output)
            output.flush()
        } catch (e: IOException) {
            // The device gave up on it, as it does when it seeks, or the session ended.
        } catch (e: Exception) {
            Log.w(TAG, "Cast server request failed", e)
        } finally {
            clients -= client
            closeQuietly(client)
        }
    }

    private class Request(val method: String, val path: String, val headers: Map<String, String>)

    // The request line and headers, up to the blank line; null for a connection that sent none.
    private fun readRequest(input: InputStream): Request? {
        val lines = ArrayList<String>()
        val line = StringBuilder()
        var total = 0
        while (true) {
            val c = input.read()
            if (c < 0) return null
            if (++total > MAX_HEADER_BYTES) throw IOException("Request headers too long")
            when (c) {
                '\r'.code -> Unit
                '\n'.code -> {
                    if (line.isEmpty()) break
                    lines += line.toString()
                    line.setLength(0)
                }
                else -> line.append(c.toChar())
            }
        }
        val parts = lines.firstOrNull()?.split(' ') ?: return null
        if (parts.size < 2) return null
        val headers = lines.drop(1).mapNotNull { header ->
            val colon = header.indexOf(':')
            if (colon <= 0) null else header.substring(0, colon).trim().lowercase() to header.substring(colon + 1).trim()
        }.toMap()
        return Request(parts[0].uppercase(), parts[1], headers)
    }

    private fun respond(request: Request, out: OutputStream) {
        if (request.method == "OPTIONS") {
            writeHead(out, 204, "No Content", emptyMap())
            return
        }
        if (request.method != "GET" && request.method != "HEAD") {
            writeHead(out, 405, "Method Not Allowed", mapOf("Allow" to "GET, HEAD, OPTIONS", "Content-Length" to "0"))
            return
        }
        val what = lookUp(request.path)
        if (what == null) {
            writeHead(out, 404, "Not Found", mapOf("Content-Length" to "0"))
            return
        }
        when (what) {
            is Served.Track -> serveFile(request, out, what.file)
            is Served.Art -> serveArt(request, out, what.uri)
        }
    }

    // What /<token>/<name> serves, if the token is this session's and the name is up.
    private fun lookUp(path: String): Served? {
        val parts = path.substringBefore('?').trimStart('/').split('/')
        if (parts.size != 2) return null
        if (!MessageDigest.isEqual(parts[0].toByteArray(), token.toByteArray())) return null
        return served[parts[1]]
    }

    private fun serveFile(request: Request, out: OutputStream, file: File) {
        val raf = try {
            RandomAccessFile(file, "r")
        } catch (e: IOException) {
            writeHead(out, 404, "Not Found", mapOf("Content-Length" to "0"))
            return
        }
        raf.use {
            val length = it.length()
            val type = mimeType(file.name)
            val asked = request.headers["range"]
            val range = asked?.let { header -> parseRange(header, length) }
            // One it can't make sense of is ignored, and the whole file sent, as HTTP has it.
            if (range == null && asked != null && pastTheEnd(asked, length)) {
                writeHead(out, 416, "Range Not Satisfiable", mapOf("Content-Range" to "bytes */$length", "Content-Length" to "0"))
                return
            }
            val (from, to) = range ?: (0L to length - 1)
            val count = (to - from + 1).coerceAtLeast(0)
            val headers = linkedMapOf(
                "Content-Type" to type,
                "Content-Length" to count.toString(),
                "Accept-Ranges" to "bytes",
            )
            if (range != null) {
                headers["Content-Range"] = "bytes $from-$to/$length"
                writeHead(out, 206, "Partial Content", headers)
            } else {
                writeHead(out, 200, "OK", headers)
            }
            if (request.method == "HEAD") return
            it.seek(from)
            val buffer = ByteArray(BUFFER_BYTES)
            var left = count
            while (left > 0) {
                val read = it.read(buffer, 0, minOf(buffer.size.toLong(), left).toInt())
                if (read < 0) break
                out.write(buffer, 0, read)
                left -= read
            }
        }
    }

    // Art is small, so it's read whole and sent whole, ranges or not.
    private fun serveArt(request: Request, out: OutputStream, uri: Uri) {
        val bytes = try {
            resolver.openInputStream(uri)?.use { it.readBytes() }
        } catch (e: Exception) {
            null
        }
        if (bytes == null) {
            writeHead(out, 404, "Not Found", mapOf("Content-Length" to "0"))
            return
        }
        writeHead(out, 200, "OK", mapOf("Content-Type" to "image/jpeg", "Content-Length" to bytes.size.toString()))
        if (request.method != "HEAD") out.write(bytes)
    }

    private fun writeHead(out: OutputStream, code: Int, reason: String, headers: Map<String, String>) {
        val head = StringBuilder("HTTP/1.1 $code $reason\r\n")
        for ((name, value) in headers) head.append(name).append(": ").append(value).append("\r\n")
        // The receiver is a web page; a media element needs none of this, but fetching does.
        head.append("Access-Control-Allow-Origin: *\r\n")
        head.append("Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n")
        head.append("Access-Control-Allow-Headers: Range\r\n")
        head.append("Access-Control-Expose-Headers: Content-Length, Content-Range, Accept-Ranges\r\n")
        head.append("Connection: close\r\n\r\n")
        out.write(head.toString().toByteArray(Charsets.ISO_8859_1))
    }

    companion object {
        private const val TAG = "kithsong"
        private const val THREADS = 6
        private const val READ_TIMEOUT_MS = 30_000
        private const val MAX_HEADER_BYTES = 16 * 1024
        private const val BUFFER_BYTES = 64 * 1024

        /**
         * A server on the phone's address towards [device] (the Cast device's), or failing
         * that the active network's IPv4 address; null if there's neither, or no port. Opens a
         * socket, so not on the main thread.
         */
        fun start(context: Context, device: InetAddress?): CastServer? {
            val address = device?.let(::addressTowards) ?: activeNetworkAddress(context) ?: return null
            return try {
                val socket = ServerSocket()
                socket.reuseAddress = true
                socket.bind(InetSocketAddress(address, 0), BACKLOG)
                CastServer(socket, context.contentResolver)
            } catch (e: IOException) {
                Log.w(TAG, "Couldn't start the cast server on $address", e)
                null
            }
        }

        private const val BACKLOG = 16

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

        /**
         * The first and last byte a Range header asks for of [length] bytes ("bytes=a-b",
         * "bytes=a-", or the last n, "bytes=-n"); null if it's none of those or out of range.
         * Of several ranges, only the first is served.
         */
        fun parseRange(header: String, length: Long): Pair<Long, Long>? {
            if (length <= 0 || !header.startsWith("bytes=", ignoreCase = true)) return null
            val spec = header.substring(6).substringBefore(',').trim()
            val dash = spec.indexOf('-')
            if (dash < 0) return null
            val first = spec.substring(0, dash).trim()
            val last = spec.substring(dash + 1).trim()
            if (first.isEmpty()) {
                val n = last.toLongOrNull()?.takeIf { it > 0 } ?: return null
                return (length - n).coerceAtLeast(0) to length - 1
            }
            val from = first.toLongOrNull() ?: return null
            val to = if (last.isEmpty()) length - 1 else last.toLongOrNull()?.coerceAtMost(length - 1) ?: return null
            return if (from >= length || to < from) null else from to to
        }

        // Whether a Range header asks for bytes from past the end of [length] bytes.
        private fun pastTheEnd(header: String, length: Long): Boolean {
            if (!header.startsWith("bytes=", ignoreCase = true)) return false
            val from = header.substring(6).substringBefore(',').substringBefore('-').trim().toLongOrNull() ?: return false
            return from >= length
        }

        /** The type a Cast device plays a track file as, by its extension. */
        fun mimeType(name: String): String = when (name.substringAfterLast('.', "").lowercase()) {
            "flac" -> "audio/flac"
            "mp3" -> "audio/mpeg"
            "opus", "ogg" -> "audio/ogg"
            "wav" -> "audio/wav"
            "aif", "aiff" -> "audio/aiff"
            else -> "application/octet-stream"
        }

        private fun closeQuietly(socket: Socket) {
            try {
                socket.close()
            } catch (_: IOException) {
            }
        }
    }
}
