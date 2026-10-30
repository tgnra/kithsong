package ng.kithso.android.playback

import android.net.Uri
import android.os.Bundle
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.Track
import java.io.File

// A queue item is a Track in a MediaItem: what the notification shows in the metadata, and
// the rest in its extras, so the UI gets the same Track back from the session.

private const val TRACK_NO = "track_no"
private const val ART_UID = "art_uid"
private const val ART_LABEL = "art_label"
private const val ART_FILE = "art_file"
private const val ART_VERSION = "art_version"
private const val ALBUM_UID = "album_uid"
private const val ARTIST_UID = "artist_uid"

/**
 * The file goes in the request metadata as well as the URI, because a MediaItem crossing from
 * a MediaController to the session may lose its URI but keeps the request metadata.
 */
fun Track.toMediaItem(): MediaItem {
    val uri = path?.let { Uri.fromFile(File(it)) }
    val extras = Bundle().apply {
        putInt(TRACK_NO, trackNo)
        putArtRef(art)
        putString(ALBUM_UID, albumUid)
        putString(ARTIST_UID, artistUid)
    }
    return MediaItem.Builder()
        .setMediaId(uid)
        .setUri(uri)
        .setRequestMetadata(MediaItem.RequestMetadata.Builder().setMediaUri(uri).setExtras(extras).build())
        .setMediaMetadata(
            MediaMetadata.Builder()
                .setTitle(title)
                .setArtist(artist)
                .setAlbumTitle(album)
                .setTrackNumber(trackNo.takeIf { it > 0 })
                .setDurationMs(durationMs)
                .setMediaType(MediaMetadata.MEDIA_TYPE_MUSIC)
                .setIsPlayable(true)
                .setIsBrowsable(false)
                .setExtras(extras)
                .build(),
        )
        .build()
}

/** The file to play. */
val MediaItem.path: String?
    get() = localConfiguration?.uri?.path ?: requestMetadata.mediaUri?.path

fun MediaItem.toTrack(): Track {
    val extras = mediaMetadata.extras ?: requestMetadata.extras ?: Bundle.EMPTY
    return Track(
        uid = mediaId,
        title = mediaMetadata.title?.toString().orEmpty(),
        artist = mediaMetadata.artist?.toString().orEmpty(),
        album = mediaMetadata.albumTitle?.toString().orEmpty(),
        trackNo = extras.getInt(TRACK_NO),
        durationMs = mediaMetadata.durationMs ?: 0,
        art = mediaMetadata.artRef ?: ArtRef(mediaId, mediaMetadata.title?.toString().orEmpty()),
        path = path,
        albumUid = extras.getString(ALBUM_UID),
        artistUid = extras.getString(ARTIST_UID),
    )
}

/** Puts [art] in a MediaMetadata's extras, where [artRef] finds it. */
fun Bundle.putArtRef(art: ArtRef) {
    putString(ART_UID, art.uid)
    putString(ART_LABEL, art.label)
    putString(ART_FILE, art.file)
    putLong(ART_VERSION, art.version)
}

/** The item's album art, for [ArtBitmapLoader]. */
val MediaMetadata.artRef: ArtRef?
    get() {
        val extras = extras ?: return null
        val uid = extras.getString(ART_UID) ?: return null
        return ArtRef(uid, extras.getString(ART_LABEL).orEmpty(), extras.getString(ART_FILE), extras.getLong(ART_VERSION))
    }
