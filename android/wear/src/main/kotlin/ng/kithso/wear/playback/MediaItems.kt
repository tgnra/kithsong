package ng.kithso.wear.playback

import android.net.Uri
import android.os.Bundle
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import ng.kithso.core.TrackInfo
import ng.kithso.playback.path
import java.io.File

// A queue item is a TrackInfo in a MediaItem: what the watch's media controls show in the
// metadata, and the rest in its extras, so the UI gets the same TrackInfo back from the session.

private const val TRACK_NO = "track_no"
private const val DISC_NO = "disc_no"
private const val YEAR = "year"
private const val COVER_DIR = "cover_dir"
private const val ALBUM_UID = "album_uid"
private const val ARTIST_UID = "artist_uid"

/**
 * The file goes in the request metadata as well as the URI (see [path]). [coverFile] is the
 * album's cover, for the media controls, if it has one.
 */
fun TrackInfo.toMediaItem(coverFile: String?): MediaItem {
    val uri = path?.let { Uri.fromFile(File(it)) }
    val extras = Bundle().apply {
        putInt(TRACK_NO, trackNo)
        putInt(DISC_NO, discNo)
        putInt(YEAR, year)
        putString(COVER_DIR, coverDir)
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
                .setDiscNumber(discNo.takeIf { it > 0 })
                .setDurationMs(durationMs.toLong())
                .setArtworkUri(coverFile?.let { Uri.fromFile(File(it)) })
                .setMediaType(MediaMetadata.MEDIA_TYPE_MUSIC)
                .setIsPlayable(true)
                .setIsBrowsable(false)
                .setExtras(extras)
                .build(),
        )
        .build()
}

fun MediaItem.toTrackInfo(): TrackInfo {
    val extras = mediaMetadata.extras ?: requestMetadata.extras ?: Bundle.EMPTY
    return TrackInfo(
        uid = mediaId,
        title = mediaMetadata.title?.toString().orEmpty(),
        trackNo = extras.getInt(TRACK_NO),
        discNo = extras.getInt(DISC_NO),
        durationMs = (mediaMetadata.durationMs ?: 0L).toInt(),
        path = path,
        artist = mediaMetadata.artist?.toString(),
        album = mediaMetadata.albumTitle?.toString(),
        coverDir = extras.getString(COVER_DIR),
        artistUid = extras.getString(ARTIST_UID),
        year = extras.getInt(YEAR),
        albumUid = extras.getString(ALBUM_UID),
    )
}
