package ng.kithso.playback

import androidx.media3.common.MediaItem

/**
 * The file a queue item plays: its URI, else the request metadata's. An app puts the file in
 * both when it makes the item, because a MediaItem crossing from a MediaController to the
 * session may lose its URI but keeps the request metadata.
 */
val MediaItem.path: String?
    get() = localConfiguration?.uri?.path ?: requestMetadata.mediaUri?.path
