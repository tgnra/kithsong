package ng.kithso.android.cast

import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.max
import kotlin.math.sin
import kotlin.math.sqrt

/** One of the background's glows: its centre as fractions of the picture, its size and strength beside the app's. */
class Glow(var x: Float, var y: Float, var size: Float = 1f, var strength: Float = 1f)

/**
 * Moves the background's glows to the music. Each glow follows a band of the sound, from the
 * bass to the treble: it swells with how loud its band is, and more as that jumps, as on a beat.
 * All of them wander slowly around their corners, faster as the music is louder. Loudness is
 * beside the loudest lately, so quiet music moves it too. Swells come up over a few frames and go
 * down slowly, and are small: the picture never flashes.
 *
 * Give it the sound in order ([hear]), and take [glows] after each frame's worth ([advance]).
 */
class MusicMotion(private val rate: Int = STREAM_RATE, homes: List<Pair<Float, Float>>) {
    val glows = homes.map { (x, y) -> Glow(x, y) }
    private val homes = homes.map { (x, y) -> x to y }

    // One-pole low-passes at the edges between the bands, so each band is the difference of two.
    private val coefficients = EDGES_HZ.map { 1f - exp(-2 * PI * it / rate).toFloat() }.toFloatArray()
    private val lows = FloatArray(EDGES_HZ.size)
    private val sums = DoubleArray(BANDS)
    private var heard = 0

    private val levels = FloatArray(BANDS)
    private val loudest = FloatArray(BANDS)
    private val slow = FloatArray(BANDS)
    private val swell = FloatArray(BANDS)
    private var energy = 0f
    private var phase = 0.0

    /** Interleaved stereo [samples], [frames] of them from [offset]. */
    fun hear(samples: ShortArray, offset: Int, frames: Int) {
        for (i in offset until offset + frames) {
            val x = (samples[i * 2] + samples[i * 2 + 1]) / 65536f
            var below = 0f
            for (b in lows.indices) {
                lows[b] += coefficients[b] * (x - lows[b])
                val band = lows[b] - below
                below = lows[b]
                sums[b] += (band * band).toDouble()
            }
            val top = x - below
            sums[BANDS - 1] += (top * top).toDouble()
        }
        heard += frames
    }

    /** Moves on by [seconds], with what was heard since the last call. */
    fun advance(seconds: Float) {
        var total = 0f
        var loudestOfAll = 0f
        for (b in 0 until BANDS) {
            levels[b] = if (heard == 0) 0f else sqrt(sums[b] / heard).toFloat()
            sums[b] = 0.0
            loudest[b] = max(levels[b], loudest[b] * exp(-seconds / LOUDEST_S))
            loudestOfAll = max(loudestOfAll, loudest[b])
        }
        for (b in 0 until BANDS) {
            // The edges leak: a band with next to nothing of its own stays still beside the rest.
            val beside = levels[b] / max(FLOOR, max(loudest[b], loudestOfAll * LEAK))
            val jump = ((beside - slow[b]) * 1.6f).coerceIn(0f, 1f)
            slow[b] += (beside - slow[b]) * (1f - exp(-seconds / SLOW_S))
            val target = 0.4f * beside + 0.6f * jump
            swell[b] += (target - swell[b]) * (1f - exp(-seconds / if (target > swell[b]) RISE_S else FALL_S))
            total += beside
        }
        heard = 0
        energy += (total / BANDS - energy) * (1f - exp(-seconds / ENERGY_S))
        phase += seconds * (IDLE_SPEED + LOUD_SPEED * energy)
        for (i in glows.indices) {
            val glow = glows[i]
            val (x, y) = homes[i]
            val s = swell[i % BANDS]
            // Two turns of unlike lengths each way, so the wander doesn't look like a circle.
            val p = phase + i * 1.7
            val wanderX = 0.6 * sin(p) + 0.4 * sin(p * 0.53 + i)
            val wanderY = 0.6 * cos(p * 0.87) + 0.4 * cos(p * 0.41 + 2.0 * i)
            // A swell leans in towards the middle too.
            glow.x = x + WANDER_X * wanderX.toFloat() + (0.5f - x) * LEAN * s
            glow.y = y + WANDER_Y * wanderY.toFloat() + (0.5f - y) * LEAN * s
            glow.size = 0.7f + 0.65f * s
            glow.strength = 0.65f + 0.55f * s
        }
    }

    /** How far glow [i] has swollen, 0 to 1. For tests. */
    internal fun swellOf(i: Int) = abs(swell[i])

    private companion object {
        val EDGES_HZ = doubleArrayOf(150.0, 600.0, 2500.0)
        const val BANDS = 4
        // About -54 dB: below this it's silence, not quiet music.
        const val FLOOR = 0.002f
        const val LEAK = 0.06f
        const val LOUDEST_S = 8f
        const val SLOW_S = 0.6f
        const val RISE_S = 0.08f
        const val FALL_S = 0.35f
        const val ENERGY_S = 0.5f
        // Turns of the wander, in radians a second.
        const val IDLE_SPEED = 0.12f
        const val LOUD_SPEED = 0.6f
        const val WANDER_X = 0.12f
        const val WANDER_Y = 0.16f
        const val LEAN = 0.3f
    }
}
