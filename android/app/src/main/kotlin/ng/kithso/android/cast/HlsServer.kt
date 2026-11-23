package ng.kithso.android.cast

import java.io.IOException
import java.io.OutputStream
import java.net.InetAddress
import java.net.Socket
import java.util.Locale
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

/** A segment of the stream, the [number]th. */
class HlsSegment(val number: Long, val data: ByteArray, val us: Long)

/** The playlist of what's being streamed now: the last few segments, with no end. */
fun mediaPlaylist(segments: List<HlsSegment>, targetSeconds: Int, name: (HlsSegment) -> String) = buildString {
    append("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-INDEPENDENT-SEGMENTS\n")
    append("#EXT-X-TARGETDURATION:$targetSeconds\n")
    append("#EXT-X-MEDIA-SEQUENCE:${segments.firstOrNull()?.number ?: 0}\n")
    for (segment in segments) {
        append(String.format(Locale.ROOT, "#EXTINF:%.5f,\n%s\n", segment.us / 1e6, name(segment)))
    }
}

/**
 * A live stream over HTTP (HLS), for the Cast device: [url] is a playlist that names one other,
 * which names the last few segments [add]ed. What's asked for before there's enough to play is
 * held until there is, as a playlist without segments is an error to the device.
 *
 * [onSent] hears how fast segments get to the device: bytes, the ms they took, and whether there's
 * more of the segment to go, on the thread that sends it.
 */
class HlsServer(
    address: InetAddress,
    private val targetSeconds: Int,
    private val onSent: (Long, Long, Boolean) -> Unit = { _, _, _ -> },
) : LocalServer(address), CastStream {
    private val monitor = ReentrantLock()
    private val changed = monitor.newCondition()
    // Under monitor.
    private val segments = ArrayDeque<HlsSegment>()
    private var count = 0L
    private var variant: String? = null

    override val url = "$base/$secret/kithsong.m3u8"
    override val contentType get() = PLAYLIST_TYPE

    init {
        listen()
    }

    /** What the stream is: [codecs] as in RFC 6381, and the [picture], as HLS says it, if any. The playlists wait for this. */
    fun describe(codecs: String, bitRate: Int, picture: String? = null) = monitor.withLock {
        variant = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-INDEPENDENT-SEGMENTS\n" +
            "#EXT-X-STREAM-INF:BANDWIDTH=$bitRate,CODECS=\"$codecs\"${picture?.let { ",$it" }.orEmpty()}\n" +
            "stream.m3u8\n"
        changed.signalAll()
    }

    fun add(data: ByteArray, us: Long) = monitor.withLock {
        segments += HlsSegment(count++, data, us)
        // A device may still be fetching what has just left the playlist.
        while (segments.size > LISTED + KEPT) segments.removeFirst()
        changed.signalAll()
    }

    override fun close() {
        super.close()
        monitor.withLock {
            segments.clear()
            changed.signalAll()
        }
    }

    override fun respond(client: Socket, method: String, path: String, out: OutputStream) {
        val name = path.substringAfterLast('/')
        if (!samePath(path.substringBeforeLast('/', ""), "/$secret")) {
            writeHead(out, "404 Not Found", "Content-Length: 0\r\n")
            return
        }
        when (method) {
            "GET", "HEAD" -> Unit
            // A web page asking to fetch it, as the receiver is one.
            "OPTIONS" -> {
                writeHead(
                    out, "204 No Content",
                    "${CORS}Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\nAccess-Control-Allow-Headers: *\r\n" +
                        "Access-Control-Allow-Private-Network: true\r\nContent-Length: 0\r\n",
                )
                return
            }
            else -> {
                writeHead(out, "405 Method Not Allowed", "Allow: GET, HEAD, OPTIONS\r\nContent-Length: 0\r\n")
                return
            }
        }
        val body = when (name) {
            "kithsong.m3u8" -> ready()?.first?.toByteArray()
            "stream.m3u8" -> ready()?.second?.let { listed -> mediaPlaylist(listed, targetSeconds) { "${it.number}.ts" }.toByteArray() }
            else -> name.removeSuffix(".ts").toLongOrNull()?.let { number ->
                monitor.withLock { segments.firstOrNull { it.number == number }?.data }
            }
        }
        val segment = name.endsWith(".ts")
        // Small, so how long writing takes is how long it takes to get there.
        if (segment) client.sendBufferSize = SEND_BUFFER
        val type = if (segment) "video/mp2t" else PLAYLIST_TYPE
        when {
            body != null -> {
                writeHead(out, "200 OK", "${CORS}Content-Type: $type\r\nCache-Control: no-store\r\nContent-Length: ${body.size}\r\n")
                if (method == "GET") if (segment) send(body, out) else out.write(body)
            }
            // A playlist that didn't come in time; otherwise there's no such segment.
            !segment && !closed -> writeHead(out, "503 Service Unavailable", "${CORS}Content-Length: 0\r\n")
            else -> writeHead(out, "404 Not Found", "${CORS}Content-Length: 0\r\n")
        }
    }

    private fun send(body: ByteArray, out: OutputStream) {
        val start = System.nanoTime()
        var at = 0
        var told = false
        val tell = { more: Boolean ->
            // What's written last may only have got as far as the buffer.
            val gone = at - SEND_BUFFER.toLong()
            if (gone > 0) onSent(gone, (System.nanoTime() - start) / 1_000_000, more)
        }
        try {
            while (at < body.size) {
                val size = minOf(SEND_PART, body.size - at)
                out.write(body, at, size)
                out.flush()
                at += size
                // Already slower than the stream plays: that can't wait for the end of it.
                if (!told && at < body.size && System.nanoTime() - start > targetSeconds * 2_000_000_000L) {
                    told = true
                    tell(true)
                }
            }
            if (!told) tell(false)
        } catch (e: IOException) {
            // The device gave up on it, which it may do for being slow; if it wasn't, it says nothing.
            if (!told && !closed && System.nanoTime() - start > targetSeconds * 1_000_000_000L) tell(true)
            throw e
        }
    }

    // The first playlist and the segments of the second, once there's enough to play.
    private fun ready(): Pair<String, List<HlsSegment>>? = monitor.withLock {
        var left = TimeUnit.MILLISECONDS.toNanos(WAIT_MS)
        while (!closed) {
            val variant = variant
            if (variant != null && count >= START) return Pair(variant, segments.takeLast(LISTED))
            if (left <= 0) return null
            left = changed.awaitNanos(left)
        }
        null
    }

    private companion object {
        const val PLAYLIST_TYPE = "application/x-mpegURL"
        const val CORS = "Access-Control-Allow-Origin: *\r\n"
        /** Segments in the playlist, and how many more are kept after they've left it. */
        const val LISTED = 6
        const val KEPT = 3
        /** How many there are before it's played: a player starts three from the end. */
        const val START = 3
        const val SEND_BUFFER = 64 * 1024
        const val SEND_PART = 16 * 1024
        const val WAIT_MS = 12_000L
    }
}
