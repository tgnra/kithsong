package ng.kithso.playback

import android.os.Bundle
import androidx.media3.common.MediaItem
import ng.kithso.core.CorePlayer

private const val PLAY_CONTEXT = "play_context"
private const val PLAY_CONTEXT_UID = "play_context_uid"

/**
 * The file a queue item plays: its URI, else the request metadata's. An app puts the file in
 * both when it makes the item, because a MediaItem crossing from a MediaController to the
 * session may lose its URI but keeps the request metadata.
 */
val MediaItem.path: String?
    get() = localConfiguration?.uri?.path ?: requestMetadata.mediaUri?.path

/** Puts [origin] in a queue item's request metadata extras, where [playOrigin] finds it. */
fun Bundle.putPlayOrigin(origin: CorePlayer.Origin) {
    putInt(PLAY_CONTEXT, origin.context.ordinal)
    putString(PLAY_CONTEXT_UID, origin.contextUid)
}

/** What the item's queue is, for the play log; all a queue's items say the same. */
val MediaItem.playOrigin: CorePlayer.Origin
    get() {
        val extras = requestMetadata.extras ?: return CorePlayer.Origin()
        val context = CorePlayer.PlayContext.entries.getOrNull(extras.getInt(PLAY_CONTEXT)) ?: CorePlayer.PlayContext.Unknown
        return CorePlayer.Origin(context, extras.getString(PLAY_CONTEXT_UID))
    }
