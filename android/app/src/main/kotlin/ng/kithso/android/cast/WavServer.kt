package ng.kithso.android.cast

import android.content.Context
import android.net.ConnectivityManager
import android.util.Log
import java.io.BufferedOutputStream
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
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.security.SecureRandom
import kotlin.math.floor
import kotlin.math.roundToInt

/** The stream's rate, whatever the phone's audio device runs at. It's 16-bit stereo. */
const val STREAM_RATE = 48000

/**
 * A WAV header for 16-bit stereo at [rate] whose lengths are the largest there are: the file
 * never ends.
 */
fun wavHeader(rate: Int = STREAM_RATE): ByteArray = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN).apply {
    put("RIFF".toByteArray(Charsets.US_ASCII))
    putInt(-1) // 0xffffffff
    put("WAVEfmt ".toByteArray(Charsets.US_ASCII))
    putInt(16)
    putShort(1) // PCM
    putShort(2)
    putInt(rate)
    putInt(rate * 4)
    putShort(4)
    putShort(16)
    put("data".toByteArray(Charsets.US_ASCII))
    putInt(-1)
}.array()

/**
 * Interleaved 16-bit stereo from any rate to [outRate], by straight lines between frames: plenty
 * for a phone's 44.1 or 48 kHz into a stream at 48. Keeps its place from one block to the next,
 * so blocks join without a seam; a new rate starts afresh.
 */
class Resampler(private val outRate: Int = STREAM_RATE) {
    private var inRate = 0
    // Where the next frame out falls, in frames of the next block in: -1 to 0 is between the
    // last block's last frame and the next block's first.
    private var place = 0.0
    private val last = IntArray(2)

    /** The [frames] frames of [input], at [rate], at [outRate]. */
    fun process(input: ShortArray, frames: Int, rate: Int): ShortArray {
        if (frames <= 0 || rate <= 0) return ShortArray(0)
        if (rate != inRate) {
            inRate = rate
            place = 0.0
        }
        val out: ShortArray
        if (rate == outRate) {
            out = input.copyOf(frames * 2)
        } else {
            val step = rate.toDouble() / outRate
            out = ShortArray(2 * maxOf(0, ((frames - 1 - place) / step).toInt() + 1))
            var n = 0
            var t = place
            while (t < frames - 1 && n < out.size) {
                val i = floor(t).toInt()
                val f = t - i
                for (c in 0..1) {
                    val a = if (i < 0) last[c] else input[i * 2 + c].toInt()
                    val b = input[(i + 1) * 2 + c].toInt()
                    out[n++] = (a + (b - a) * f).roundToInt().toShort()
                }
                t += step
            }
            place = t - frames
            if (n < out.size) return out.copyOf(n)
        }
        last[0] = input[(frames - 1) * 2].toInt()
        last[1] = input[(frames - 1) * 2 + 1].toInt()
        return out
    }
}

/**
 * Says how much silence a stream owes, so it goes on in real time while nothing plays: none
 * until [quietMs] have passed without sound, then all the time since the last sound, a second
 * at most at once (a phone that slept shouldn't push the stream that far behind).
 */
class SilencePacer(private val quietMs: Long, nowMs: Long, private val rate: Int = STREAM_RATE) {
    private var soundMs = nowMs
    private var owed = 0L // frames of silence already given since soundMs

    /** Sound was written at [nowMs]. */
    fun sound(nowMs: Long) {
        soundMs = nowMs
        owed = 0
    }

    /** Frames of silence to write at [nowMs]. */
    fun silence(nowMs: Long): Int {
        val quiet = nowMs - soundMs
        if (quiet < quietMs) return 0
        val due = quiet * rate / 1000 - owed
        owed += due
        return due.coerceIn(0, rate.toLong()).toInt()
    }
}

/**
 * The phone's sound as one endless WAV file over HTTP, for the Cast device: [url] is on the
 * phone's address on the network the device is on, under a path no one else on it can guess,
 * as the sound is the user's and a request for it takes the stream from the device. Answers
 * nothing else.
 *
 * The newest request for it gets the stream, from where the sound is then: a Cast device makes
 * one, and another only to start over. What [source] gives (as CorePlayer.readTap: interleaved
 * 16-bit stereo and its rate, 0 frames for none yet, -1 for no more) goes out at [STREAM_RATE];
 * while it gives nothing, as while paused, silence goes out instead in real time, so the
 * stream never stalls. [source] is only called from one thread at a time, and never once
 * [close] has returned.
 */
class WavServer(address: InetAddress, private val source: (ShortArray, IntArray) -> Int) : AutoCloseable {
    private val socket = ServerSocket().apply {
        reuseAddress = true
        bind(InetSocketAddress(address, 0), BACKLOG)
    }
    private val path = "/" + ByteArray(16).also(SecureRandom()::nextBytes).joinToString("") { "%02x".format(it) } + ".wav"
    private val lock = Any()
    @Volatile private var closed = false
    // The connection that has the stream, and every one open, under lock.
    private var current: Socket? = null
    private val clients = HashSet<Socket>()

    val url = "http://${hostOf(address)}:${socket.localPort}$path"

    init {
        thread("kithsong-cast-accept") { accept() }
    }

    /** Stops listening and drops every connection; [source] isn't called after this. */
    override fun close() {
        val open = synchronized(lock) {
            closed = true
            current = null
            clients.toList()
        }
        closeQuietly(socket)
        open.forEach(::closeQuietly)
    }

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
            val asked = request.getOrNull(1)?.substringBefore('?').orEmpty()
            val method = request[0].uppercase()
            if (!MessageDigest.isEqual(asked.toByteArray(), path.toByteArray())) {
                writeHead(out, "404 Not Found", "Content-Length: 0\r\n")
            } else if (method == "HEAD") {
                writeHead(out, "200 OK", STREAM_HEADERS)
            } else if (method != "GET") {
                writeHead(out, "405 Method Not Allowed", "Allow: GET, HEAD\r\nContent-Length: 0\r\n")
            } else {
                val previous = synchronized(lock) {
                    if (closed) return
                    current.also { current = client }
                }
                previous?.let(::closeQuietly)
                writeHead(out, "200 OK", STREAM_HEADERS)
                out.write(wavHeader())
                out.flush()
                stream(client, out)
            }
            out.flush()
        } catch (e: IOException) {
            // The device let go of it, a newer request took over, or the session ended.
        } finally {
            synchronized(lock) {
                clients -= client
                if (current === client) current = null
            }
            closeQuietly(client)
        }
    }

    // Sound, or silence while there's none, until another request takes over or it's closed.
    private fun stream(client: Socket, out: OutputStream) {
        val samples = ShortArray(CHUNK_FRAMES * 2)
        val rate = IntArray(1)
        val resampler = Resampler()
        val pacer = SilencePacer(QUIET_MS, nowMs())
        var bytes = ByteArray(0)
        val silence = ByteArray(STREAM_RATE * 4)
        while (true) {
            // Under the lock, so close() waits for a read under way, and none starts after it.
            val frames = synchronized(lock) { if (closed || current !== client) -1 else source(samples, rate) }
            if (frames < 0) return
            if (frames > 0) {
                val sound = resampler.process(samples, frames, rate[0].takeIf { it > 0 } ?: STREAM_RATE)
                if (bytes.size < sound.size * 2) bytes = ByteArray(sound.size * 2)
                ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).asShortBuffer().put(sound)
                out.write(bytes, 0, sound.size * 2)
                pacer.sound(nowMs())
            } else {
                val quiet = pacer.silence(nowMs())
                if (quiet > 0) out.write(silence, 0, quiet * 4)
            }
            out.flush()
            // A full read means there's more waiting.
            if (frames < CHUNK_FRAMES) Thread.sleep(TICK_MS)
        }
    }

    companion object {
        private const val TAG = "kithsong"
        private const val BACKLOG = 8
        private const val REQUEST_TIMEOUT_MS = 10_000
        private const val MAX_REQUEST_BYTES = 16 * 1024
        private const val CHUNK_FRAMES = 4096
        private const val TICK_MS = 20L
        // Silence only goes out once nothing has come for this long: sound comes every tick
        // while playing, a little late at times.
        private const val QUIET_MS = 200L
        private const val STREAM_HEADERS = "Content-Type: audio/wav\r\nCache-Control: no-store\r\n" +
            "Access-Control-Allow-Origin: *\r\n"

        /**
         * A server on the phone's address towards [device] (the Cast device's), or failing
         * that the active network's IPv4 address; null if there's neither, or no port. Opens a
         * socket, so not on the main thread.
         */
        fun start(context: Context, device: InetAddress?, source: (ShortArray, IntArray) -> Int): WavServer? {
            val address = device?.let(::addressTowards) ?: activeNetworkAddress(context) ?: return null
            return try {
                WavServer(address, source)
            } catch (e: IOException) {
                Log.w(TAG, "Couldn't start the cast stream on $address", e)
                null
            }
        }

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

        private fun nowMs() = System.nanoTime() / 1_000_000

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

        private fun writeHead(out: OutputStream, status: String, headers: String) {
            out.write("HTTP/1.1 $status\r\n${headers}Connection: close\r\n\r\n".toByteArray(Charsets.ISO_8859_1))
        }

        private fun thread(name: String, block: () -> Unit) = Thread(block, name).apply { isDaemon = true }.start()

        private fun closeQuietly(socket: java.io.Closeable) {
            try {
                socket.close()
            } catch (_: IOException) {
            }
        }
    }
}
