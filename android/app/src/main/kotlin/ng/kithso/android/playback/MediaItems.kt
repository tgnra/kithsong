package ng.kithso.android.playback

import android.content.Context
import android.net.Uri
import android.os.Bundle
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Track
import ng.kithso.core.CorePlayer
import ng.kithso.playback.path
import ng.kithso.playback.putPlayOrigin
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

/** What playing this collection is, for the play log. */
val CollectionRef.origin: CorePlayer.Origin
    get() = CorePlayer.Origin(
        when (kind) {
            CollectionRef.Kind.Album -> CorePlayer.PlayContext.Album
            CollectionRef.Kind.Artist -> CorePlayer.PlayContext.Artist
            CollectionRef.Kind.Playlist -> CorePlayer.PlayContext.Playlist
            CollectionRef.Kind.AlbumGroup -> CorePlayer.PlayContext.AlbumGroup
            CollectionRef.Kind.Station -> CorePlayer.PlayContext.Radio
        },
        uid,
    )

/**
 * [tracks] as queue items, of a queue that is [origin]. An album's tracks share a cover, and
 * finding its URI touches the disk, so each cover's is found once.
 */
fun queueItems(context: Context, tracks: List<Track>, origin: CorePlayer.Origin): List<MediaItem> {
    val artwork = HashMap<ArtRef, Uri>()
    return tracks.map { it.toMediaItem(artwork.getOrPut(it.art) { ArtProvider.uriFor(context, it.art) }, origin) }
}

/**
 * A station's queue items for [uids] and [tracks], one track for each uid, or null for one the
 * library no longer has: it still needs an item, as the player's indexes have to stay in step
 * with the core's, and the core will skip it.
 */
fun stationItems(context: Context, uids: List<String>, tracks: List<Track?>, origin: CorePlayer.Origin): List<MediaItem> {
    val items = queueItems(context, tracks.filterNotNull(), origin).iterator()
    return uids.indices.map { i ->
        if (tracks[i] != null) items.next() else MediaItem.Builder().setMediaId(uids[i]).build()
    }
}

/**
 * The file goes in the request metadata as well as the URI, because a MediaItem crossing from
 * a MediaController to the session may lose its URI but keeps the request metadata (see
 * [path]). [artwork] isn't what art is loaded from ([artRef] is), but Media3 leaves extras out
 * when it compares items, so it's what makes an item with a new cover a changed one: without
 * it the player and the notification would go on showing the old cover.
 */
private fun Track.toMediaItem(artwork: Uri, origin: CorePlayer.Origin): MediaItem {
    val uri = path?.let { Uri.fromFile(File(it)) }
    val extras = Bundle().apply {
        putInt(TRACK_NO, trackNo)
        putArtRef(art)
        putString(ALBUM_UID, albumUid)
        putString(ARTIST_UID, artistUid)
        putPlayOrigin(origin)
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
                .setArtworkUri(artwork)
                .setMediaType(MediaMetadata.MEDIA_TYPE_MUSIC)
                .setIsPlayable(true)
                .setIsBrowsable(false)
                .setExtras(extras)
                .build(),
        )
        .build()
}

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
