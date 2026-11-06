package ng.kithso.android.playback

import android.util.AtomicFile
import android.util.Log
import ng.kithso.core.CorePlayer
import java.io.File
import java.io.IOException

/** A queue as it was left: its tracks by uid, the one playing and how far in, and what it is. */
data class SavedQueue(val uids: List<String>, val index: Int, val positionMs: Long, val origin: CorePlayer.Origin) {
    /** As [decode] reads it: a line of the rest, then a uid a line. */
    fun encode(): String = buildString {
        append(index).append(' ').append(positionMs).append(' ').append(origin.context.ordinal)
        append(' ').append(origin.contextUid.orEmpty()).append('\n')
        uids.forEach { append(it).append('\n') }
    }

    /**
     * This queue with only the tracks [kept] says are still here. If the one playing has gone,
     * it starts from the start of the next, or with none left of the last.
     */
    fun keeping(kept: (Int) -> Boolean): SavedQueue? {
        val left = uids.indices.filter(kept)
        if (left.isEmpty()) return null
        val current = left.indexOf(index)
        val start = if (current >= 0) current else left.count { it < index }.coerceAtMost(left.lastIndex)
        return copy(uids = left.map { uids[it] }, index = start, positionMs = if (current >= 0) positionMs else 0)
    }

    companion object {
        /** What [encode] wrote, or null if that isn't what [text] is. */
        fun decode(text: String): SavedQueue? {
            val lines = text.lines().filter(String::isNotEmpty)
            val head = lines.firstOrNull()?.split(' ') ?: return null
            if (head.size != 4 || lines.size < 2) return null
            val index = head[0].toIntOrNull() ?: return null
            val position = head[1].toLongOrNull() ?: return null
            val context = head[2].toIntOrNull()?.let(CorePlayer.PlayContext.entries::getOrNull) ?: return null
            val uids = lines.drop(1)
            if (index !in uids.indices) return null
            return SavedQueue(uids, index, position, CorePlayer.Origin(context, head[3].ifEmpty { null }))
        }
    }
}

/**
 * The queue kept in [file] for after the process has gone: Android can end it at any time, so
 * it's written as the queue goes along rather than at the end.
 */
class QueueMemory(file: File) {
    private val file = AtomicFile(file)

    @Synchronized
    fun read(): SavedQueue? = try {
        SavedQueue.decode(file.readFully().decodeToString())
    } catch (e: IOException) {
        null // none yet
    }

    /** Keeps [queue]; with null, there's nothing to go back to. */
    @Synchronized
    fun write(queue: SavedQueue?) {
        if (queue == null) {
            file.delete()
            return
        }
        val out = try {
            file.startWrite()
        } catch (e: IOException) {
            Log.w(TAG, "Couldn't save the queue", e)
            return
        }
        try {
            out.write(queue.encode().encodeToByteArray())
            file.finishWrite(out)
        } catch (e: IOException) {
            Log.w(TAG, "Couldn't save the queue", e)
            file.failWrite(out)
        }
    }

    private companion object {
        const val TAG = "kithsong"
    }
}
