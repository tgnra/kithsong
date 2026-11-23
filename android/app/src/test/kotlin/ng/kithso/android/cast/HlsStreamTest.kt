package ng.kithso.android.cast

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.InetAddress
import java.net.URI
import kotlin.concurrent.thread
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.sin

class HlsStreamTest {
    // What a transport stream carries: each PID's payloads, joined, one for each payload start.
    private fun parse(stream: ByteArray): Map<Int, List<ByteArray>> {
        assertEquals(0, stream.size % 188)
        val found = HashMap<Int, ArrayList<ByteArrayOutputStream>>()
        val counters = HashMap<Int, Int>()
        for (at in stream.indices step 188) {
            assertEquals(0x47, stream[at].toInt())
            val pid = (stream[at + 1].toInt() and 0x1f shl 8) or (stream[at + 2].toInt() and 0xff)
            val flags = stream[at + 3].toInt()
            val counter = flags and 0xf
            counters[pid]?.let { assertEquals("pid $pid", it + 1 and 0xf, counter) }
            counters[pid] = counter
            var from = at + 4
            if (flags and 0x20 != 0) from += 1 + (stream[at + 4].toInt() and 0xff)
            assertTrue(from <= at + 188)
            val list = found.getOrPut(pid) { ArrayList() }
            if (stream[at + 1].toInt() and 0x40 != 0) list += ByteArrayOutputStream()
            list.last().write(stream, from, at + 188 - from)
        }
        return found.mapValues { (_, list) -> list.map { it.toByteArray() } }
    }

    private fun time(pes: ByteArray): Long {
        val b = LongArray(5) { pes[9 + it].toLong() and 0xff }
        return (b[0] shr 1 and 7 shl 30) or (b[1] shl 22) or (b[2] shr 1 shl 15) or (b[3] shl 7) or (b[4] shr 1)
    }

    @Test
    fun crcIsMpegs() {
        assertEquals(0x0376e6e7, TsMuxer.crc32("123456789".toByteArray()))
    }

    @Test
    fun tablesCheckOut() {
        val out = ByteArrayOutputStream()
        TsMuxer().tables(out)
        val parsed = parse(out.toByteArray())
        for (pid in listOf(TsMuxer.PAT_PID, TsMuxer.PMT_PID)) {
            val table = parsed.getValue(pid).single()
            val length = (table[2].toInt() and 0xf shl 8) or (table[3].toInt() and 0xff)
            // Over a section and its own CRC, the CRC comes to nothing.
            assertEquals(0, TsMuxer.crc32(table.copyOfRange(1, 4 + length)))
        }
    }

    @Test
    fun framesComeBackAsTheyWere() {
        val muxer = TsMuxer()
        val out = ByteArrayOutputStream()
        // Sizes either side of where packets fill and stuffing starts.
        val sizes = (1..400) + listOf(5000, 70_000)
        sizes.forEachIndexed { i, size ->
            muxer.video(out, ByteArray(size) { (it * 7 + i).toByte() }, i * 33_333L, key = i == 0)
            muxer.audio(out, ByteArray(minOf(size, 8000)) { (it * 3 + i).toByte() }, i * 21_333L)
        }
        val parsed = parse(out.toByteArray())
        val videos = parsed.getValue(TsMuxer.VIDEO_PID)
        val audios = parsed.getValue(TsMuxer.AUDIO_PID)
        assertEquals(sizes.size, videos.size)
        assertEquals(sizes.size, audios.size)
        sizes.forEachIndexed { i, size ->
            assertEquals(mpegTime(i * 33_333L), time(videos[i]))
            // After the header and the access unit delimiter.
            assertArrayEquals("video $size", ByteArray(size) { (it * 7 + i).toByte() }, videos[i].copyOfRange(14 + 6, videos[i].size))
            val audioSize = minOf(size, 8000)
            assertEquals(mpegTime(i * 21_333L), time(audios[i]))
            assertArrayEquals("audio $size", ByteArray(audioSize) { (it * 3 + i).toByte() }, audios[i].copyOfRange(14 + 7, audios[i].size))
            val adts = audios[i].copyOfRange(14, 21).map { it.toInt() and 0xff }
            assertEquals(audioSize + 7, (adts[3] and 3 shl 11) or (adts[4] shl 3) or (adts[5] shr 5))
        }
    }

    @Test
    fun mpegTimeWrapsAt33Bits() {
        assertEquals(126_000, mpegTime(0))
        assertEquals(126_000 + 90_000, mpegTime(1_000_000))
        val day = 27L * 3600 * 1_000_000
        assertTrue(mpegTime(day) in 0 until (1L shl 33))
    }

    @Test
    fun segmentsStartOnKeyFrames() {
        val segments = ArrayList<Pair<ByteArray, Long>>()
        val segmenter = Segmenter(2_000_000) { data, us -> segments += data to us }
        // Key frames every half second; sound comes well ahead of the video that goes with it.
        var sound = 0L
        for (i in 0 until 200) {
            while (sound < i * 33_333L + 300_000) {
                segmenter.audio(ByteArray(300), sound)
                sound += 21_333
            }
            segmenter.video(ByteArray(2000), i * 33_333L, key = i % 15 == 0)
        }
        assertEquals(3, segments.size)
        var videos = 0
        for ((data, us) in segments) {
            assertEquals(2_000_000.0, us.toDouble(), 2000.0)
            val parsed = parse(data)
            assertTrue(parsed.containsKey(TsMuxer.PAT_PID) && parsed.containsKey(TsMuxer.PMT_PID))
            assertEquals(60, parsed.getValue(TsMuxer.VIDEO_PID).size)
            // The sound of those two seconds.
            assertEquals(93.75, parsed.getValue(TsMuxer.AUDIO_PID).size.toDouble(), 1.0)
            assertEquals(mpegTime(videos * 33_333L), time(parsed.getValue(TsMuxer.VIDEO_PID).first()))
            videos += 60
        }
    }

    @Test
    fun soundAloneIsSegmentedByTime() {
        val segments = ArrayList<Pair<ByteArray, Long>>()
        val segmenter = Segmenter(2_000_000, hasVideo = false) { data, us -> segments += data to us }
        for (i in 0 until 300) segmenter.audio(ByteArray(340) { i.toByte() }, i * 1024L * 1_000_000 / 48_000)
        assertEquals(3, segments.size)
        for ((data, us) in segments) {
            assertEquals(2_005_333.0, us.toDouble(), 2.0)
            val parsed = parse(data)
            assertEquals(setOf(TsMuxer.PAT_PID, TsMuxer.PMT_PID, TsMuxer.AUDIO_PID), parsed.keys)
            assertEquals(94, parsed.getValue(TsMuxer.AUDIO_PID).size)
            // The clock comes with the sound.
            val table = parsed.getValue(TsMuxer.PMT_PID).single()
            assertEquals(TsMuxer.AUDIO_PID, (table[9].toInt() and 0x1f shl 8) or (table[10].toInt() and 0xff))
            val length = (table[2].toInt() and 0xf shl 8) or (table[3].toInt() and 0xff)
            assertEquals(0, TsMuxer.crc32(table.copyOfRange(1, 4 + length)))
            assertEquals(0x50, data[188 * 2 + 5].toInt())
        }
    }

    @Test
    fun serverTimesASlowDevice() {
        val told = java.util.Collections.synchronizedList(ArrayList<Triple<Long, Long, Boolean>>())
        HlsServer(InetAddress.getLoopbackAddress(), 1, onSent = { bytes, ms, more -> told += Triple(bytes, ms, more) }).use { server ->
            server.describe("mp4a.40.2", 1_000_000)
            server.add(ByteArray(8_000_000), 1_000_000)
            val url = URI(server.url)
            java.net.Socket().use { socket ->
                socket.receiveBufferSize = 32 * 1024
                socket.connect(java.net.InetSocketAddress(url.host, url.port))
                socket.getOutputStream().write("GET ${url.path.substringBeforeLast('/')}/0.ts HTTP/1.1\r\n\r\n".toByteArray())
                val until = System.nanoTime() + 3_000_000_000
                val part = ByteArray(16 * 1024)
                // About 1.6 MB a second at most.
                while (System.nanoTime() < until) {
                    socket.getInputStream().read(part)
                    Thread.sleep(10)
                }
                val (bytes, ms, more) = told.single()
                assertTrue(more)
                assertTrue("$bytes in $ms", ms >= 2000 && bytes * 1000 / ms < 2_000_000)
            }
        }
    }

    private val levels = listOf(2_756_000, 1_456_000, 856_000)

    // [seconds] of 2 second segments, at the level's rate, over a network of [bps]; the time after.
    private fun StreamRate.run(bps: Int, seconds: Int, from: Long): Long {
        var now = from
        repeat(seconds / 2) {
            val bytes = levels[level] / 4L
            val ms = bytes * 8000 / bps
            now += maxOf(ms, 2000)
            sent(bytes, ms, now)
        }
        return now
    }

    @Test
    fun aGoodNetworkKeepsThePicture() {
        val rate = StreamRate(levels, 0)
        rate.run(20_000_000, 600, 0)
        assertEquals(0, rate.level)
        assertFalse(rate.tooSlow)
    }

    @Test
    fun aSlowNetworkGetsWhatFits() {
        val rate = StreamRate(levels, 0)
        rate.run(2_500_000, 8, 0)
        assertEquals(1, rate.level)
        rate.run(2_500_000, 300, 20_000)
        assertEquals(1, rate.level)
        val slower = StreamRate(levels, 0)
        slower.run(1_300_000, 60, 0)
        assertEquals(2, slower.level)
        assertFalse(slower.tooSlow)
    }

    @Test
    fun oneSlowSegmentStillUnderWayIsEnough() {
        val rate = StreamRate(levels, 0)
        rate.sent(200_000, 4000, 4000, unfinished = true)
        assertEquals(2, rate.level)
        assertFalse(rate.tooSlow)
    }

    @Test
    fun onlyAWhileOfTooSlowGivesUpThePicture() {
        val rate = StreamRate(levels, 0)
        val now = rate.run(700_000, 8, 0)
        assertEquals(2, rate.level)
        assertFalse(rate.tooSlow)
        rate.run(700_000, 30, now)
        assertTrue(rate.tooSlow)
        // A bad moment that passes doesn't.
        val blip = StreamRate(levels, 0)
        blip.run(20_000_000, 120, blip.run(700_000, 8, 0))
        assertFalse(blip.tooSlow)
    }

    @Test
    fun itComesBackUpInSteps() {
        val rate = StreamRate(levels, 0)
        var now = rate.run(1_000_000, 8, 0)
        assertEquals(2, rate.level)
        now = rate.run(20_000_000, 50, now)
        assertEquals(2, rate.level)
        now = rate.run(20_000_000, 20, now)
        assertEquals(1, rate.level)
        now = rate.run(20_000_000, 70, now)
        assertEquals(0, rate.level)
        // Down again after that, the wait is twice as long.
        now = rate.run(1_500_000, 8, now)
        val down = rate.level
        assertTrue(down > 0)
        now = rate.run(20_000_000, 90, now)
        assertEquals(down, rate.level)
        rate.run(20_000_000, 60, now)
        assertEquals(down - 1, rate.level)
    }

    @Test
    fun stallsBringItDownAStepAtATime() {
        val rate = StreamRate(levels, 0)
        rate.stalled(5_000) // still starting
        rate.stalled(20_000)
        assertEquals(0, rate.level)
        rate.stalled(90_000) // too long after the last
        assertEquals(0, rate.level)
        rate.stalled(100_000)
        assertEquals(1, rate.level)
        rate.stalled(105_000) // what was sent before the change
        rate.stalled(120_000)
        rate.stalled(130_000)
        assertEquals(2, rate.level)
        rate.stalled(150_000)
        assertFalse(rate.tooSlow)
        rate.stalled(160_000)
        assertTrue(rate.tooSlow)
    }

    @Test
    fun playlistListsTheLatest() {
        val text = mediaPlaylist(listOf(HlsSegment(7, ByteArray(0), 2_000_000), HlsSegment(8, ByteArray(0), 1_966_667)), 2) { "${it.number}.ts" }
        assertEquals(
            listOf(
                "#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-INDEPENDENT-SEGMENTS", "#EXT-X-TARGETDURATION:2", "#EXT-X-MEDIA-SEQUENCE:7",
                "#EXTINF:2.00000,", "7.ts", "#EXTINF:1.96667,", "8.ts",
            ),
            text.trim().lines(),
        )
    }

    @Test
    fun serverHoldsThePlaylistUntilThereIsEnough() {
        HlsServer(InetAddress.getLoopbackAddress(), 2).use { server ->
            val url = URI(server.url)
            assertTrue(url.path.matches(Regex("/[0-9a-f]{32}/kithsong\\.m3u8")))
            var master: String? = null
            val asking = thread { master = fetch(server.url).second }
            server.describe("avc1.640028,mp4a.40.2", 5_000_000, "RESOLUTION=1280x720")
            server.add(byteArrayOf(0), 2_000_000)
            server.add(byteArrayOf(1), 2_000_000)
            asking.join(300)
            assertNull(master)
            server.add(byteArrayOf(2), 2_000_000)
            asking.join(5000)
            assertTrue(master!!.contains("CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION=1280x720"))
            val at = server.url.substringBeforeLast('/')
            assertTrue(fetch("$at/stream.m3u8").second.trim().endsWith("2.ts"))
            assertEquals("\u0001", fetch("$at/1.ts").second)
            assertEquals(404, fetch("$at/9.ts").first)
            assertEquals(404, fetch(server.url.replace(Regex("/[0-9a-f]{32}/"), "/${"0".repeat(32)}/")).first)
            // The oldest go, out of the playlist first.
            for (i in 3..20) server.add(byteArrayOf(i.toByte()), 2_000_000)
            val listed = fetch("$at/stream.m3u8").second
            assertTrue(listed.contains("MEDIA-SEQUENCE:15") && listed.trim().endsWith("20.ts"))
            assertEquals(200, fetch("$at/14.ts").first)
            assertEquals(404, fetch("$at/1.ts").first)
            val options = (URI(server.url).toURL().openConnection() as HttpURLConnection).apply { requestMethod = "OPTIONS" }
            assertEquals(204, options.responseCode)
            assertEquals("*", options.getHeaderField("Access-Control-Allow-Origin"))
        }
    }

    @Test
    fun closingTheServerLetsGoOfWhoIsWaiting() {
        val server = HlsServer(InetAddress.getLoopbackAddress(), 2)
        val asking = thread { runCatching { fetch(server.url) } }
        Thread.sleep(200)
        server.close()
        asking.join(3000)
        assertFalse(asking.isAlive)
    }

    @Test
    fun clockFillsGapsToRealTime() {
        val clock = StreamClock(quietMs = 200, startMs = 0)
        // Two seconds of sound, with a gap of 150 ms that's too short to fill.
        clock.sound(48_000, 1000)
        clock.sound(48_000, 2150)
        assertEquals(0, clock.silence(2300))
        // Quiet now: silence up to real time, the old gap with it.
        assertEquals((2350 - 2000) * 48, clock.silence(2350))
        assertEquals(20 * 48, clock.silence(2370))
        assertEquals(2370L * 48, clock.frames)
        // After a long sleep, a second at most, and real time goes on from there.
        assertEquals(48_000, clock.silence(100_000))
        assertEquals(960, clock.silence(100_020))
    }

    @Test
    fun clockAheadOfRealTimeWaits() {
        val clock = StreamClock(quietMs = 200, startMs = 0)
        clock.sound(48_000, 500)
        assertEquals(0, clock.silence(900))
        assertEquals(4800, clock.silence(1100))
    }

    private val homes = listOf(0.1f to 0.05f, 0.9f to 0.2f, 0.15f to 0.75f, 0.9f to 0.95f)

    private fun tone(hz: Double, from: Int, frames: Int, gain: Double) =
        ShortArray(frames * 2) { (sin((from + it / 2) * 2 * PI * hz / STREAM_RATE) * gain * 32767).toInt().toShort() }

    @Test
    fun eachGlowFollowsItsBand() {
        for ((hz, glow) in listOf(40.0 to 0, 10_000.0 to 3)) {
            val motion = MusicMotion(homes = homes)
            repeat(30) {
                motion.hear(tone(hz, it * 1600, 1600, 0.5), 0, 1600)
                motion.advance(1 / 30f)
            }
            // The other end's moves with what leaks into its band, but much less.
            assertTrue("$hz: ${motion.swellOf(glow)}", motion.swellOf(glow) > 0.35f)
            assertTrue("$hz: ${motion.swellOf(3 - glow)}", motion.swellOf(3 - glow) < motion.swellOf(glow) * 0.6f)
        }
    }

    @Test
    fun aBeatSwellsMoreThanTheSameSoundHeld() {
        val motion = MusicMotion(homes = homes)
        var top = 0f
        var held = 0f
        for (i in 0 until 150) {
            motion.hear(tone(60.0, i * 1600, 1600, 0.5), 0, 1600)
            motion.advance(1 / 30f)
            top = maxOf(top, motion.swellOf(0))
            held = motion.swellOf(0)
        }
        assertTrue("$top then $held", top > held * 1.5f)
    }

    @Test
    fun silenceSettlesAndStillDrifts() {
        val motion = MusicMotion(homes = homes)
        repeat(30) {
            motion.hear(tone(60.0, it * 1600, 1600, 0.9), 0, 1600)
            motion.advance(1 / 30f)
        }
        repeat(120) {
            motion.hear(ShortArray(3200), 0, 1600)
            motion.advance(1 / 30f)
        }
        assertTrue((0..3).all { motion.swellOf(it) < 0.01f })
        val before = motion.glows[0].x
        repeat(60) { motion.advance(1 / 30f) }
        assertTrue(abs(motion.glows[0].x - before) > 0.001f)
    }

    @Test
    fun nothingJumps() {
        // The loudest there is, beating: no glow goes far, or brightens much, from one frame to the next.
        val motion = MusicMotion(homes = homes)
        var last = motion.glows.map { Glow(it.x, it.y, it.size, it.strength) }
        val random = java.util.Random(1)
        for (i in 0 until 600) {
            val loud = i % 15 < 4
            motion.hear(ShortArray(3200) { if (loud) (random.nextInt(65536) - 32768).toShort() else 0 }, 0, 1600)
            motion.advance(1 / 30f)
            if (i > 0) motion.glows.forEachIndexed { g, glow ->
                assertTrue(abs(glow.x - last[g].x) < 0.06f && abs(glow.y - last[g].y) < 0.06f)
                assertTrue(abs(glow.strength - last[g].strength) < 0.2f)
            }
            last = motion.glows.map { Glow(it.x, it.y, it.size, it.strength) }
        }
    }

    @Test
    fun codecComesFromTheParameterSets() {
        val sets = byteArrayOf(0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28, 1, 2, 3, 0, 0, 0, 1, 0x68, 1, 2)
        assertEquals("avc1.640028", HlsStream.h264Codec(sets))
        assertNull(HlsStream.h264Codec(byteArrayOf(0, 0, 0, 1, 0x68, 1, 2, 3, 4, 5)))
    }

    private fun fetch(url: String): Pair<Int, String> {
        val connection = URI(url).toURL().openConnection() as HttpURLConnection
        val code = connection.responseCode
        return code to (if (code < 400) connection.inputStream.use { String(it.readBytes(), Charsets.ISO_8859_1) } else "")
    }
}
