package ng.kithso.android.cast

import android.os.SystemClock
import ng.kithso.core.CorePlayer

/** The core's rule for when a play counts (mc_play_counts): half the track or four minutes, whichever is less. */
fun playCounts(durationMs: Long, listenedMs: Long): Boolean {
    val needed = if (durationMs > 0 && durationMs / 2 < MAX_PLAY_MS) durationMs / 2 else MAX_PLAY_MS
    return listenedMs >= needed
}

private const val MAX_PLAY_MS = 4 * 60 * 1000L

/**
 * Plays while casting. The core decides when a play counts by the audio it hears itself, and a
 * cast plays none of that, so this does the same from the Cast device's state: time listened
 * is the time it spent playing the track (buffering, pausing and seeking don't add to it), and
 * a play counts by the core's rule ([playCounts]). It goes to [played] when it does, for play
 * counts and scrobbling, as CoreMediaPlayer's onPlayed would, and to [nowPlaying] as it starts
 * playing.
 *
 * The play log's rows still come from the core, which CastPlayer moves along item by item as the
 * device plays, so each play there opens and closes a row at the same moments as playing on the
 * phone would, and the row knows what the play was from (an album added to up next, a station).
 * But the core heard none of it, so its rows say nothing was listened to: [patch] puts in what
 * this saw of the same play, matched by track. A row for a file the core couldn't open stays as
 * the core made it; any other with nothing to match is one the device never played (where the
 * core was put as a cast began, or as it played out), and goes.
 *
 * Use it on the main thread.
 */
class CastPlays(
    private val nowPlaying: (String) -> Unit,
    private val played: (String, Long) -> Unit,
    private val wallMs: () -> Long = System::currentTimeMillis,
    private val monoMs: () -> Long = SystemClock::elapsedRealtime,
) {
    /** A play that has ended, to [patch] the core's row for it with. */
    private class Ended(val uid: String, val startedMs: Long, val listenedMs: Long, val end: CorePlayer.PlayEnd)

    /**
     * Whether a cast is under way: while it is, the core's own counted plays and now playing
     * (it hears nothing, so it has none) aren't passed on, and its rows are [patch]ed.
     */
    var casting = false

    private var uid: String? = null
    private var durationMs = 0L
    private var startedMs = 0L
    private var listenedMs = 0L
    private var counted = false
    // When it last started playing (monotonic), or -1 while it isn't.
    private var playingSince = -1L
    private var announced = false
    private val ended = ArrayDeque<Ended>()

    /** A play of track [uid] begins, not playing yet; one under way must have [end]ed. */
    fun start(uid: String, durationMs: Long) {
        this.uid = uid
        this.durationMs = durationMs
        startedMs = wallMs()
        listenedMs = 0
        counted = false
        playingSince = -1
        announced = false
    }

    /** Whether the device is playing the track now; called as that changes, and every second or so. */
    fun update(playing: Boolean) {
        val uid = uid ?: return
        val now = monoMs()
        if (playingSince >= 0) listenedMs += now - playingSince
        playingSince = if (playing) now else -1
        if (playing && !announced) {
            announced = true
            // As the core's, the play began when it was first heard.
            if (listenedMs == 0L) startedMs = wallMs()
            nowPlaying(uid)
        } else if (!playing) {
            // Announced again once it plays on, as CoreMediaPlayer does after a pause.
            announced = false
        }
        if (!counted && playCounts(durationMs, listenedMs)) {
            counted = true
            played(uid, startedMs)
        }
    }

    /** The play under way, if any, ended as [end]. */
    fun end(end: CorePlayer.PlayEnd) {
        val uid = uid ?: return
        update(playing = false)
        if (ended.size == MAX_ENDED) ended.removeFirst()
        ended.addLast(Ended(uid, startedMs, listenedMs, end))
        this.uid = null
    }

    /**
     * The core's row [event] as it goes in the play log: as it is but while [casting], then
     * with what this saw of the same play, or null if the device didn't play it.
     */
    fun patch(event: CorePlayer.PlayEvent): CorePlayer.PlayEvent? {
        if (!casting || event.end == CorePlayer.PlayEnd.Failed) return event
        val i = ended.indexOfFirst { it.uid == event.trackUid }
        if (i < 0) return null
        val play = ended[i]
        // Those before it ended without a row of the core's: skipped through faster than the
        // core opened them, as it does on the phone too.
        repeat(i + 1) { ended.removeFirst() }
        return event.copy(startedMs = play.startedMs, listenedMs = play.listenedMs, end = play.end)
    }

    /** Forgets everything, at the end of a cast, so no row of the phone's own is taken for a cast's. */
    fun clear() {
        uid = null
        ended.clear()
    }

    private companion object {
        const val MAX_ENDED = 16
    }
}
