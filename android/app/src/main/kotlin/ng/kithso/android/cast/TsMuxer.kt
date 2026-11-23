package ng.kithso.android.cast

import java.io.OutputStream

/** The 90 kHz clock of MPEG timestamps, for a time in the stream. It wraps after 26 hours. */
fun mpegTime(us: Long): Long = (us * 9 / 100 + TIME_BASE) and 0x1ffffffffL

// So the clock reference, which is sent a little behind, never starts below zero.
private const val TIME_BASE = 126_000L

/**
 * H.264 video and AAC audio, or only the audio without [hasVideo], as an MPEG transport stream of
 * one program. Call [tables] where a segment starts; counters go on from one segment to the next,
 * as they're one stream.
 */
class TsMuxer(private val hasVideo: Boolean = true, private val sampleRate: Int = STREAM_RATE) {
    private val counters = HashMap<Int, Int>()
    private val packet = ByteArray(PACKET)

    /** The tables that say what's in the stream. */
    fun tables(out: OutputStream) {
        section(
            out, PAT_PID,
            byteArrayOf(
                0x00, // program association
                0xb0.toByte(), 13, 0x00, 0x01, 0xc1.toByte(), 0x00, 0x00,
                0x00, 0x01, (0xe0 or (PMT_PID shr 8)).toByte(), PMT_PID.toByte(),
            ),
        )
        // The clock comes with the video, if there's any.
        val clockPid = if (hasVideo) VIDEO_PID else AUDIO_PID
        var streams = byteArrayOf(0x0f, (0xe0 or (AUDIO_PID shr 8)).toByte(), AUDIO_PID.toByte(), 0xf0.toByte(), 0x00) // AAC in ADTS
        if (hasVideo) {
            streams = byteArrayOf(0x1b, (0xe0 or (VIDEO_PID shr 8)).toByte(), VIDEO_PID.toByte(), 0xf0.toByte(), 0x00) + streams // H.264
        }
        section(
            out, PMT_PID,
            byteArrayOf(
                0x02, // program map
                0xb0.toByte(), (13 + streams.size).toByte(), 0x00, 0x01, 0xc1.toByte(), 0x00, 0x00,
                (0xe0 or (clockPid shr 8)).toByte(), clockPid.toByte(),
                0xf0.toByte(), 0x00,
            ) + streams,
        )
    }

    /** A frame of H.264, in start codes, with its parameter sets first if it's a [key] frame. */
    fun video(out: OutputStream, frame: ByteArray, us: Long, key: Boolean) {
        val time = mpegTime(us)
        pes(out, VIDEO_PID, 0xe0, ACCESS_UNIT, frame, time, clock(time), key)
    }

    /** A frame of stereo AAC-LC, as the encoder gives it. */
    fun audio(out: OutputStream, frame: ByteArray, us: Long) {
        val time = mpegTime(us)
        pes(out, AUDIO_PID, 0xc0, adts(frame.size, sampleRate), frame, time, if (hasVideo) -1 else clock(time), random = true)
    }

    private fun clock(time: Long) = (time - CLOCK_BEHIND) and 0x1ffffffffL

    private fun pes(out: OutputStream, pid: Int, stream: Int, prefix: ByteArray, frame: ByteArray, time: Long, clock: Long, random: Boolean) {
        val length = 8 + prefix.size + frame.size
        // Video's may be any length, said as 0.
        val said = if (pid == VIDEO_PID || length > 0xffff) 0 else length
        val header = byteArrayOf(
            0, 0, 1, stream.toByte(), (said shr 8).toByte(), said.toByte(),
            0x84.toByte(), 0x80.toByte(), 5, // aligned data, with a presentation time
            (0x21 or (time shr 29).toInt().and(0x0e)).toByte(),
            (time shr 22).toByte(),
            (time shr 14).toInt().or(1).toByte(),
            (time shr 7).toByte(),
            (time shl 1).toInt().or(1).toByte(),
        )
        val data = ByteArray(header.size + prefix.size + frame.size)
        header.copyInto(data)
        prefix.copyInto(data, header.size)
        frame.copyInto(data, header.size + prefix.size)
        packets(out, pid, data, clock, random)
    }

    private fun packets(out: OutputStream, pid: Int, data: ByteArray, clock: Long, random: Boolean) {
        var at = 0
        while (at < data.size) {
            val first = at == 0
            // The adaptation field's bytes after its length byte; -1 for no field.
            var adaptation = if (!first) -1 else if (clock >= 0) 7 else if (random) 1 else -1
            val room = PACKET - 4 - (adaptation + 1)
            val size = minOf(room, data.size - at)
            adaptation += room - size // stuffing, so the last packet is whole
            val counter = counters[pid] ?: 0
            counters[pid] = counter + 1 and 0xf
            packet[0] = 0x47
            packet[1] = ((if (first) 0x40 else 0) or (pid shr 8)).toByte()
            packet[2] = pid.toByte()
            packet[3] = ((if (adaptation >= 0) 0x30 else 0x10) or counter).toByte()
            if (adaptation >= 0) {
                packet[4] = adaptation.toByte()
                packet.fill(0xff.toByte(), 5, 5 + adaptation)
                if (adaptation > 0) packet[5] = ((if (first && random) 0x40 else 0) or (if (first && clock >= 0) 0x10 else 0)).toByte()
                if (first && clock >= 0) {
                    packet[6] = (clock shr 25).toByte()
                    packet[7] = (clock shr 17).toByte()
                    packet[8] = (clock shr 9).toByte()
                    packet[9] = (clock shr 1).toByte()
                    packet[10] = ((clock shl 7).toInt() and 0x80 or 0x7e).toByte()
                    packet[11] = 0
                }
            }
            data.copyInto(packet, PACKET - size, at, at + size)
            out.write(packet)
            at += size
        }
    }

    private fun section(out: OutputStream, pid: Int, section: ByteArray) {
        val crc = crc32(section)
        val data = ByteArray(PACKET - 4) { 0xff.toByte() }
        data[0] = 0 // the section starts at once
        section.copyInto(data, 1)
        for (i in 0..3) data[1 + section.size + i] = (crc shr (24 - i * 8)).toByte()
        packets(out, pid, data, clock = -1, random = false)
    }

    companion object {
        const val PACKET = 188
        const val PAT_PID = 0
        const val PMT_PID = 0x1000
        const val VIDEO_PID = 0x100
        const val AUDIO_PID = 0x101
        // Half a second: how long the receiver may hold a frame before showing it.
        private const val CLOCK_BEHIND = 45_000L
        private val ACCESS_UNIT = byteArrayOf(0, 0, 0, 1, 9, 0xf0.toByte())

        /** The ADTS header of a [size] byte frame of stereo AAC-LC. */
        fun adts(size: Int, sampleRate: Int): ByteArray {
            val rate = RATES.indexOf(sampleRate)
            require(rate >= 0) { "No AAC at $sampleRate Hz" }
            val length = size + 7
            return byteArrayOf(
                0xff.toByte(), 0xf1.toByte(),
                ((1 shl 6) or (rate shl 2)).toByte(), // LC
                ((2 shl 6) or (length shr 11)).toByte(), // two channels
                (length shr 3).toByte(),
                ((length and 7 shl 5) or 0x1f).toByte(),
                0xfc.toByte(),
            )
        }

        private val RATES = intArrayOf(96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000)

        /** MPEG's CRC-32: the usual polynomial, but most significant bit first. */
        fun crc32(data: ByteArray): Int {
            var crc = -1
            for (b in data) {
                crc = crc xor (b.toInt() shl 24)
                repeat(8) { crc = if (crc < 0) (crc shl 1) xor 0x04c11db7 else crc shl 1 }
            }
            return crc
        }
    }
}

/**
 * Cuts encoded video and audio into transport stream segments for HLS, each starting on a key
 * frame and about [segmentUs] long, and gives each, whole, to [onSegment] with its length. Frames
 * go in as the encoders give them, each kind in order; without [hasVideo], there's only audio.
 */
class Segmenter(private val segmentUs: Long, private val hasVideo: Boolean = true, private val onSegment: (ByteArray, Long) -> Unit) {
    private class Frame(val data: ByteArray, val us: Long, val key: Boolean)

    private val muxer = TsMuxer(hasVideo)
    private val videos = ArrayDeque<Frame>()
    private val audios = ArrayDeque<Frame>()
    private val segment = java.io.ByteArrayOutputStream(1 shl 20)
    private var startUs = -1L

    /** See [TsMuxer.video]. */
    fun video(frame: ByteArray, us: Long, key: Boolean) {
        videos += Frame(frame, us, key)
        write()
    }

    fun audio(frame: ByteArray, us: Long) {
        if (!hasVideo) {
            // Any frame of audio can start a segment.
            if (startUs < 0 || us - startUs >= segmentUs) cut(us)
            muxer.audio(segment, frame, us)
            return
        }
        audios += Frame(frame, us, key = false)
        write()
    }

    // In order of time, as far as both kinds go: what one has ahead of the other waits.
    private fun write() {
        while (videos.isNotEmpty() && audios.isNotEmpty()) {
            if (audios.first().us < videos.first().us) {
                val frame = audios.removeFirst()
                // What's before the first key frame has no segment to go in.
                if (startUs >= 0) muxer.audio(segment, frame.data, frame.us)
                continue
            }
            val frame = videos.removeFirst()
            if (startUs < 0 && !frame.key) continue
            if (frame.key && (startUs < 0 || frame.us - startUs >= segmentUs * 19 / 20)) cut(frame.us)
            muxer.video(segment, frame.data, frame.us, frame.key)
        }
    }

    private fun cut(us: Long) {
        if (startUs >= 0) onSegment(segment.toByteArray(), us - startUs)
        segment.reset()
        startUs = us
        muxer.tables(segment)
    }
}
