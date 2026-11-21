package ng.kithso.android.cast

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.net.InetAddress
import java.net.Socket
import java.net.URI
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.abs

class WavStreamTest {
    @Test
    fun headerIsEndless16BitStereo() {
        val header = ByteBuffer.wrap(wavHeader()).order(ByteOrder.LITTLE_ENDIAN)
        assertEquals(44, header.capacity())
        assertEquals("RIFF", ascii(header, 0))
        assertEquals(-1, header.getInt(4))
        assertEquals("WAVE", ascii(header, 8))
        assertEquals("fmt ", ascii(header, 12))
        assertEquals(16, header.getInt(16))
        assertEquals(1, header.getShort(20).toInt()) // PCM
        assertEquals(2, header.getShort(22).toInt())
        assertEquals(48000, header.getInt(24))
        assertEquals(48000 * 4, header.getInt(28))
        assertEquals(4, header.getShort(32).toInt())
        assertEquals(16, header.getShort(34).toInt())
        assertEquals("data", ascii(header, 36))
        assertEquals(-1, header.getInt(40))
    }

    @Test
    fun theStreamsRateGoesThroughAsItIs() {
        val input = ShortArray(200) { (it * 7).toShort() }
        val resampler = Resampler()
        assertArrayEquals(input.copyOf(100), resampler.process(input, 50, 48000))
        assertEquals(0, resampler.process(input, 0, 48000).size)
    }

    @Test
    fun anotherRateIsResampledWithoutSeams() {
        // A ramp at 44.1 kHz, in blocks of odd sizes: what comes out is the same ramp at 48,
        // the right length overall, and smooth where the blocks join.
        val frames = 44100
        val ramp = ShortArray(frames * 2) { (it / 2 % 20000 - 10000).toShort() }
        val resampler = Resampler()
        val out = ArrayList<Short>()
        var at = 0
        var size = 37
        while (at < frames) {
            val n = minOf(size, frames - at)
            out += resampler.process(ramp.copyOfRange(at * 2, (at + n) * 2), n, 44100).asList()
            at += n
            size = size * 3 % 1013 + 1
        }
        val outFrames = out.size / 2
        assertTrue("$outFrames frames", abs(outFrames - 48000) <= 2)
        val step = 44100.0 / 48000
        for (i in 0 until outFrames - 1) {
            val left = out[i * 2].toInt()
            assertEquals(left, out[i * 2 + 1].toInt()) // both channels alike, as they went in
            val t = i * step
            if (t.toInt() % 20000 < 19999) assertEquals("frame $i", t % 20000 - 10000, left.toDouble(), 1.0)
        }
    }

    @Test
    fun aNewRateStartsAfresh() {
        val resampler = Resampler()
        resampler.process(ShortArray(20) { 1000 }, 10, 44100)
        val out = resampler.process(ShortArray(20) { 5 }, 10, 96000)
        assertEquals(10, out.size) // every other of the ten frames, none from before
        assertTrue(out.all { it.toInt() == 5 })
    }

    @Test
    fun silenceKeepsRealTimeOnceQuiet() {
        val pacer = SilencePacer(quietMs = 200, nowMs = 0)
        assertEquals(0, pacer.silence(100))
        assertEquals(0, pacer.silence(199))
        // All of the quiet so far, then only what's passed since.
        assertEquals(9600, pacer.silence(200))
        assertEquals(960, pacer.silence(220))
        assertEquals(0, pacer.silence(220))
        var total = 9600 + 960
        for (t in 240..10_000L step 20) total += pacer.silence(t)
        assertEquals(480_000, total) // ten seconds, to the frame
        // Sound starts the wait again.
        pacer.sound(10_000)
        assertEquals(0, pacer.silence(10_150))
        assertEquals(48 * 250, pacer.silence(10_250))
        // After a long sleep, a second at most at once.
        assertEquals(48000, pacer.silence(60_000))
        assertEquals(480, pacer.silence(60_010))
    }

    @Test
    fun serverStreamsToTheNewestRequestOnly() {
        val sound = ShortArray(8) { (it + 1).toShort() }
        val server = WavServer(InetAddress.getLoopbackAddress()) { out, rate ->
            sound.copyInto(out)
            rate[0] = 48000
            4
        }
        server.use {
            val url = URI(server.url)
            assertTrue(url.path.matches(Regex("/[0-9a-f]{32}\\.wav")))
            assertTrue(request(url.port, "GET /nope.wav HTTP/1.1").startsWith("HTTP/1.1 404"))
            assertTrue(request(url.port, "POST ${url.path} HTTP/1.1").startsWith("HTTP/1.1 405"))
            assertTrue(request(url.port, "HEAD ${url.path} HTTP/1.1").contains("Content-Type: audio/wav"))

            val first = open(url.port, "GET ${url.path} HTTP/1.1")
            val head = readHead(first.getInputStream())
            assertTrue(head, head.startsWith("HTTP/1.1 200") && head.contains("Content-Type: audio/wav"))
            assertArrayEquals(wavHeader(), readBytes(first.getInputStream(), 44))
            val samples = ByteBuffer.wrap(readBytes(first.getInputStream(), 16)).order(ByteOrder.LITTLE_ENDIAN)
            assertEquals((1..8).toList(), List(8) { samples.getShort().toInt() })

            // A second request takes the stream; the first one ends.
            val second = open(url.port, "GET ${url.path} HTTP/1.1")
            readHead(second.getInputStream())
            assertArrayEquals(wavHeader(), readBytes(second.getInputStream(), 44))
            first.soTimeout = 5000
            val drained = first.getInputStream().readBytes()
            assertTrue(drained.size % 2 == 0)
            first.close()
            second.close()
        }
    }

    @Test
    fun serverSendsSilenceWhileNothingPlays() {
        val server = WavServer(InetAddress.getLoopbackAddress()) { _, _ -> 0 }
        server.use {
            val url = URI(server.url)
            val socket = open(url.port, "GET ${url.path} HTTP/1.1")
            readHead(socket.getInputStream())
            readBytes(socket.getInputStream(), 44)
            val started = System.nanoTime()
            // A tenth of a second's worth of silence comes, in about that time once quiet.
            val silence = readBytes(socket.getInputStream(), 48000 * 4 / 10)
            val ms = (System.nanoTime() - started) / 1_000_000
            assertTrue(silence.all { it.toInt() == 0 })
            assertTrue("$ms ms", ms >= 150)
            socket.close()
        }
    }

    private fun ascii(buffer: ByteBuffer, at: Int) = String(buffer.array(), at, 4, Charsets.US_ASCII)

    private fun open(port: Int, line: String): Socket = Socket(InetAddress.getLoopbackAddress(), port).apply {
        soTimeout = 5000
        getOutputStream().write("$line\r\nHost: test\r\n\r\n".toByteArray())
    }

    private fun request(port: Int, line: String): String = open(port, line).use { String(it.getInputStream().readBytes()) }

    private fun readHead(input: InputStream): String {
        val head = ByteArrayOutputStream()
        while (!head.toString().endsWith("\r\n\r\n")) head.write(input.read().also { check(it >= 0) })
        return head.toString()
    }

    private fun readBytes(input: InputStream, n: Int): ByteArray {
        val bytes = ByteArray(n)
        var at = 0
        while (at < n) at += input.read(bytes, at, n - at).also { check(it >= 0) }
        return bytes
    }
}
