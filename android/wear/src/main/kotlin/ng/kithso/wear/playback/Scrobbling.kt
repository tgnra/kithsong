package ng.kithso.wear.playback

import android.util.Log
import ng.kithso.core.CoreLibrary
import ng.kithso.core.CorePlayer
import ng.kithso.core.CoreScrobbler
import ng.kithso.core.KithCoreException
import java.io.File

/**
 * What becomes of the plays the player counts (CoreMediaPlayer's onPlayed): each goes into the
 * library's play counts ([CoreLibrary.recordPlay]) and to the core's scrobbler
 * ([CoreScrobbler]) for the sync group's Last.fm account. The account arrives by sync, in the
 * sync.db in [configDir] (the folder sync is given): the watch has no sign-in and no switch of
 * its own, and the core drops plays while nobody is signed in. Use on the main thread, as
 * [library] is; the scrobbler does its work on a thread of its own.
 */
class Scrobbling(private val library: CoreLibrary, private val configDir: File) {
    private var scrobbler: CoreScrobbler? = null
    // Set after it wouldn't open, so a broken sync.db isn't tried again on every play.
    private var failed = false

    private fun open(): CoreScrobbler? {
        scrobbler?.let { return it }
        if (failed) return null
        return try {
            configDir.mkdirs()
            CoreScrobbler(library, configDir.path).also { scrobbler = it }
        } catch (e: KithCoreException) {
            Log.e(TAG, "Couldn't start the scrobbler", e)
            failed = true
            null
        }
    }

    private fun post(what: (CoreScrobbler) -> Unit) {
        val sc = open() ?: return
        try {
            what(sc)
        } catch (e: KithCoreException) {
            Log.w(TAG, "The scrobbler turned a play down", e)
        }
    }

    /** [trackUid] has started playing. */
    fun nowPlaying(trackUid: String) = post { it.nowPlaying(trackUid) }

    /** A play of [trackUid] counts, begun at [startedMs] (wall clock). */
    fun played(trackUid: String, startedMs: Long) {
        try {
            library.recordPlay(trackUid)
        } catch (e: KithCoreException) {
            Log.w(TAG, "Couldn't count a play", e)
        }
        post { it.scrobble(trackUid, startedMs) }
    }

    /** A play has ended, counted or not, for the library's play log. */
    fun ended(event: CorePlayer.PlayEvent) {
        try {
            library.logPlay(event)
        } catch (e: KithCoreException) {
            Log.w(TAG, "Couldn't log a play", e)
        }
    }

    private companion object {
        const val TAG = "kithsong"
    }
}
