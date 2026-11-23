package ng.kithso.android.cast

import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo.CodecCapabilities
import android.media.MediaCodecInfo.CodecProfileLevel
import android.media.MediaCodecInfo.EncoderCapabilities
import android.media.MediaFormat
import android.os.Bundle
import android.util.Log
import android.view.Surface
import ng.kithso.android.ui.theme.BackdropCorners
import java.io.IOException
import java.net.InetAddress
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference

/**
 * The phone's sound as a live stream (HLS): with [hasPicture], a video (H.264 and AAC) for a Cast
 * device with a screen, the art backdrop moving to the music ([MusicMotion]) behind what's playing
 * ([show]); without, the AAC alone, at a rate a poor network can take. The picture is made from
 * the sound at its own place in the stream, so the two stay together however far behind the phone
 * the device plays.
 *
 * The picture's bit rate follows what the network takes ([StreamRate]), without a break in the
 * stream. [source] is as [WavServer]'s, and silence fills in as there; it's only called from one
 * thread, and never once [close] has returned. If it can't go on as it is, it says so once, to
 * [onTrouble], on a thread of its own; it still needs closing.
 */
class HlsStream private constructor(
    address: InetAddress,
    val hasPicture: Boolean,
    private val source: (ShortArray, IntArray) -> Int,
    private val onTrouble: (HlsStream) -> Unit,
) : CastStream {
    enum class Trouble {
        /** The phone can't make it: it has no encoder for it, say. */
        Failed,
        /** The network can't take even the least picture. */
        TooSlow,
    }

    private val rate = StreamRate(PICTURE_RATES.map { it + AAC_BIT_RATE }, nowMs())
    private val server = HlsServer(address, SEGMENT_S) { bytes, ms, more ->
        rate.sent(bytes, ms, nowMs(), unfinished = more)
        checkRate()
    }
    /** What's wrong with it, if anything. */
    @Volatile var trouble: Trouble? = null
        private set
    private val lock = Any()
    @Volatile private var closed = false
    private val next = AtomicReference<CastPicture?>()
    private val firstPicture = CountDownLatch(1)

    override val url get() = server.url
    override val contentType get() = server.contentType

    /** Whether the picture has been made less of for the network. */
    val reduced get() = rate.level > 0

    init {
        Thread({ run() }, "kithsong-cast-hls").apply { isDaemon = true }.start()
    }

    /** Shows [picture] from now on in the stream. From any thread. */
    fun show(picture: CastPicture) {
        next.set(picture)
        firstPicture.countDown()
    }

    /** The device has run out of stream to play. */
    fun stalled() {
        rate.stalled(nowMs())
        checkRate()
    }

    private fun checkRate() {
        if (hasPicture && rate.tooSlow) troubled(Trouble.TooSlow)
    }

    private fun troubled(what: Trouble) {
        synchronized(lock) {
            if (closed || trouble != null) return
            trouble = what
        }
        onTrouble(this)
    }

    override fun close() {
        // Under the lock, so this waits for a read of the source under way.
        synchronized(lock) { closed = true }
        server.close()
        firstPicture.countDown()
    }

    private fun run() {
        var pipeline: Pipeline? = null
        try {
            pipeline = Pipeline()
            stream(pipeline)
        } catch (e: Exception) {
            if (!closed) {
                Log.w(TAG, "The cast's stream failed", e)
                troubled(Trouble.Failed)
            }
        } finally {
            pipeline?.release()
        }
    }

    private fun stream(pipeline: Pipeline) {
        val samples = ShortArray(CHUNK_FRAMES * 2)
        val rate = IntArray(1)
        val resampler = Resampler()
        val clock = StreamClock(QUIET_MS, nowMs())
        // So it opens on what's playing rather than on nothing, if that's soon found.
        if (hasPicture) firstPicture.await(FIRST_PICTURE_MS, TimeUnit.MILLISECONDS)
        // A player starts this far back from the newest segment, so give it that much at once.
        repeat(LEAD_CHUNKS) {
            if (closed) return
            pipeline.sound(ShortArray(CHUNK_FRAMES * 2), CHUNK_FRAMES, behind = false)
        }
        val silence = ShortArray(CHUNK_FRAMES * 2)
        // Full reads in a row: some are only what played while this started.
        var full = 0
        while (true) {
            // Under the lock, so close() waits for a read under way, and none starts after it.
            val frames = synchronized(lock) { if (closed) -1 else source(samples, rate) }
            if (frames < 0) return
            full = if (frames == CHUNK_FRAMES) full + 1 else 0
            if (frames > 0) {
                val sound = resampler.process(samples, frames, rate[0].takeIf { it > 0 } ?: STREAM_RATE)
                clock.sound(sound.size / 2, nowMs())
                pipeline.sound(sound, sound.size / 2, behind = full > BEHIND_READS)
            } else {
                var left = clock.silence(nowMs())
                while (left > 0 && !closed) {
                    val count = minOf(left, CHUNK_FRAMES)
                    pipeline.sound(silence, count, behind = false)
                    left -= count
                }
            }
            pipeline.drain()
            // A full read means there's more waiting.
            if (frames < CHUNK_FRAMES) Thread.sleep(TICK_MS)
        }
    }

    /** The encoders, and what draws for them. On the stream's thread, all of it. */
    private inner class Pipeline {
        private val info = MediaCodec.BufferInfo()
        private val segmenter = Segmenter(SEGMENT_S * 1_000_000L, hasPicture, server::add)
        private var audio: MediaCodec? = null
        private var video: MediaCodec? = null
        private var surface: Surface? = null
        private var renderer: CastRenderer? = null
        // Which of PICTURE_RATES the video encoder is at.
        private var level = 0
        private val motion = MusicMotion(STREAM_RATE, HOMES)

        // Frames of sound given so far, frames of video, and of AAC taken.
        private var heard = 0L
        private var drawn = 0L
        private var lastDrawn = -1L
        private var aacFrames = 0L
        // H.264's parameter sets, which go before each key frame.
        private var parameters = ByteArray(0)

        init {
            try {
                audio = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC).apply {
                    val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, STREAM_RATE, 2)
                    format.setInteger(MediaFormat.KEY_AAC_PROFILE, CodecProfileLevel.AACObjectLC)
                    format.setInteger(MediaFormat.KEY_BIT_RATE, if (hasPicture) AAC_BIT_RATE else LEAN_AAC_BIT_RATE)
                    format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, CHUNK_FRAMES * 4)
                    configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                    start()
                }
                if (hasPicture) startVideo() else server.describe(AAC_CODEC, LEAN_AAC_BIT_RATE * 2)
            } catch (e: Exception) {
                release()
                throw e
            }
        }

        // In High profile where the encoder has it.
        private fun startVideo() {
            var failure: Exception? = null
            for (high in listOf(true, false)) {
                val codec = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
                try {
                    val can = codec.codecInfo.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
                    check(!high || can.profileLevels.any { it.profile == CodecProfileLevel.AVCProfileHigh }) { "No High profile" }
                    val constant = can.encoderCapabilities?.isBitrateModeSupported(EncoderCapabilities.BITRATE_MODE_CBR) == true
                    val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, WIDTH, HEIGHT).apply {
                        setInteger(MediaFormat.KEY_COLOR_FORMAT, CodecCapabilities.COLOR_FormatSurface)
                        setInteger(MediaFormat.KEY_BIT_RATE, PICTURE_RATES[level])
                        setInteger(MediaFormat.KEY_FRAME_RATE, FPS)
                        setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, SEGMENT_S)
                        // Constant where it can be, as the rate is for what the network takes.
                        setInteger(
                            MediaFormat.KEY_BITRATE_MODE,
                            if (constant) EncoderCapabilities.BITRATE_MODE_CBR else EncoderCapabilities.BITRATE_MODE_VBR,
                        )
                        // Presentation order is the only order the segments know.
                        setInteger(KEY_MAX_B_FRAMES, 0)
                        setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
                        setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
                        setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
                        if (high) {
                            setInteger(MediaFormat.KEY_PROFILE, CodecProfileLevel.AVCProfileHigh)
                            setInteger(MediaFormat.KEY_LEVEL, CodecProfileLevel.AVCLevel31)
                        }
                    }
                    codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                    val surface = codec.createInputSurface().also { this.surface = it }
                    codec.start()
                    video = codec
                    renderer = CastRenderer(surface, WIDTH, HEIGHT)
                    return
                } catch (e: Exception) {
                    failure = e
                    // With it goes what was made for it.
                    renderer = null
                    video = null
                    surface?.release()
                    surface = null
                    codec.release()
                }
            }
            throw failure ?: IOException("No video")
        }

        /**
         * [count] frames of stereo [samples], and the frames of video that go with them.
         * [behind]: the sound is getting ahead of the encoder, so every other frame of video is
         * left out, to catch up.
         */
        fun sound(samples: ShortArray, count: Int, behind: Boolean) {
            var at = 0
            while (at < count && !closed) {
                // Up to the next frame of video, so it's drawn from the sound before it.
                val until = (drawn + 1) * STREAM_RATE / FPS
                val part = minOf(count - at, (until - heard).toInt())
                encode(samples, at, part)
                if (hasPicture) motion.hear(samples, at, part)
                heard += part
                at += part
                if (heard == until) frame(behind)
            }
        }

        private fun frame(behind: Boolean) {
            if (!hasPicture) {
                drawn++
                drain()
                return
            }
            motion.advance(1f / FPS)
            val number = drawn++
            if (behind && number % 2 == 1L && number % (SEGMENT_S * FPS) != 0L) return
            val renderer = renderer ?: return
            if (!renderer.fading) next.getAndSet(null)?.let(renderer::show)
            val wanted = rate.level
            if (wanted != level) {
                level = wanted
                Log.i(TAG, "The cast's picture goes to ${PICTURE_RATES[level] / 1000} kbps")
                video?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, PICTURE_RATES[level]) })
            }
            // Each segment starts on a key frame, so ask, in case its interval is inexact.
            if (number % (SEGMENT_S * FPS) == 0L && number > 0) {
                video?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
            }
            renderer.draw(motion.glows, (number - lastDrawn) / FPS.toFloat(), number * 1_000_000 / FPS)
            lastDrawn = number
            drain()
        }

        private fun encode(samples: ShortArray, offset: Int, count: Int) {
            val codec = audio ?: return
            var at = 0
            while (at < count && !closed) {
                val index = codec.dequeueInputBuffer(CODEC_WAIT_US)
                if (index < 0) {
                    drain()
                    continue
                }
                val buffer = codec.getInputBuffer(index) ?: continue
                buffer.clear()
                val shorts = buffer.order(ByteOrder.LITTLE_ENDIAN).asShortBuffer()
                val frames = minOf(count - at, shorts.remaining() / 2)
                shorts.put(samples, (offset + at) * 2, frames * 2)
                codec.queueInputBuffer(index, 0, frames * 4, (heard + at) * 1_000_000 / STREAM_RATE, 0)
                at += frames
            }
        }

        /** Takes what the encoders have ready. */
        fun drain() {
            take(audio) { data, _, config ->
                if (!config) segmenter.audio(data, aacFrames++ * AAC_FRAMES * 1_000_000 / STREAM_RATE)
            }
            take(video) { data, us, config ->
                if (config) parameters(data)
                else {
                    val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                    segmenter.video(if (key) parameters + data else data, us, key)
                }
            }
        }

        private inline fun take(codec: MediaCodec?, each: (ByteArray, Long, Boolean) -> Unit) {
            codec ?: return
            while (true) {
                val index = codec.dequeueOutputBuffer(info, 0)
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    // Where the parameter sets are said here too, they may not come as a buffer.
                    if (codec === video && parameters.isEmpty()) {
                        val format = codec.outputFormat
                        val sets = listOf("csd-0", "csd-1").mapNotNull { format.getByteBuffer(it)?.let(::bytes) }
                        if (sets.isNotEmpty()) parameters(sets.reduce(ByteArray::plus))
                    }
                    continue
                }
                if (index < 0) return
                val buffer = codec.getOutputBuffer(index)
                if (buffer != null && info.size > 0) {
                    buffer.position(info.offset).limit(info.offset + info.size)
                    each(bytes(buffer), info.presentationTimeUs, info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0)
                }
                codec.releaseOutputBuffer(index, false)
            }
        }

        private fun bytes(buffer: ByteBuffer) = ByteArray(buffer.remaining()).also { buffer.duplicate().get(it) }

        private fun parameters(sets: ByteArray) {
            parameters = sets
            val codec = h264Codec(sets) ?: return
            server.describe(
                "$codec,$AAC_CODEC",
                PICTURE_RATES[0] * 2 + AAC_BIT_RATE,
                "RESOLUTION=${WIDTH}x$HEIGHT,FRAME-RATE=$FPS.000",
            )
        }

        fun release() {
            // Each on its own, as a codec that has failed throws at any of it.
            quietly { renderer?.release() }
            for (codec in listOf(video, audio)) {
                quietly { codec?.stop() }
                quietly { codec?.release() }
            }
            quietly { surface?.release() }
        }

        private fun quietly(block: () -> Unit) {
            try {
                block()
            } catch (e: RuntimeException) {
                Log.d(TAG, "Letting go of the cast's encoders", e)
            }
        }
    }

    companion object {
        private const val TAG = "kithsong"
        private const val FPS = 30
        private const val SEGMENT_S = 2
        /** The picture's size, which a [CastPicture] is best made at. */
        const val WIDTH = 1280
        const val HEIGHT = 720
        // Bits a second of H.264, for StreamRate to pick from.
        private val PICTURE_RATES = listOf(2_500_000, 1_200_000, 600_000)
        private const val AAC_BIT_RATE = 256_000
        // Without a picture, as that's for a network that couldn't take one.
        private const val LEAN_AAC_BIT_RATE = 128_000
        private const val AAC_CODEC = "mp4a.40.2"
        private const val AAC_FRAMES = 1024
        private const val CHUNK_FRAMES = 4096
        // A little over three segments of silence.
        private const val LEAD_CHUNKS = (3 * SEGMENT_S * STREAM_RATE + STREAM_RATE / 2) / CHUNK_FRAMES
        // Three and a half seconds' worth, of the tap's five.
        private const val BEHIND_READS = 40
        private const val TICK_MS = 20L
        private const val QUIET_MS = 200L
        private const val FIRST_PICTURE_MS = 1000L
        private const val CODEC_WAIT_US = 10_000L
        // MediaFormat's is only there from Android 10; before it, it's ignored.
        private const val KEY_MAX_B_FRAMES = "max-bframes"
        // Where ArtBackdrop has its glows.
        private val HOMES = BackdropCorners.map { it.x to it.y }

        /**
         * A stream on the phone's address towards [device] (see [LocalServer.localAddress]); null
         * if there's none, or no port. Opens a socket, so not on the main thread. The encoders
         * start after, on a thread of its own ([onTrouble]).
         */
        fun start(
            context: Context,
            device: InetAddress?,
            hasPicture: Boolean,
            source: (ShortArray, IntArray) -> Int,
            onTrouble: (HlsStream) -> Unit,
        ): HlsStream? {
            val address = LocalServer.localAddress(context, device) ?: return null
            return try {
                HlsStream(address, hasPicture, source, onTrouble)
            } catch (e: IOException) {
                Log.w(TAG, "Couldn't start the cast stream on $address", e)
                null
            }
        }

        private fun nowMs() = System.nanoTime() / 1_000_000

        /** The name in RFC 6381 of the H.264 whose parameter sets these are, or null without any. */
        internal fun h264Codec(sets: ByteArray): String? {
            for (i in 0 until sets.size - 6) {
                val startCode = sets[i].toInt() == 0 && sets[i + 1].toInt() == 0 && sets[i + 2].toInt() == 1
                if (startCode && sets[i + 3].toInt() and 0x1f == 7) {
                    return "avc1.%02x%02x%02x".format(Locale.ROOT, sets[i + 4], sets[i + 5], sets[i + 6])
                }
            }
            return null
        }
    }
}
