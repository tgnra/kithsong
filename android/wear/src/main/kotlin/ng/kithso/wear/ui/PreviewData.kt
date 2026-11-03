package ng.kithso.wear.ui

import ng.kithso.core.AlbumInfo
import ng.kithso.core.NamedInfo
import ng.kithso.core.PlaylistInfo
import ng.kithso.core.TrackInfo

/** Made-up albums, tracks, playlists and album groups for the @Previews. */
internal object PreviewData {
    val albums = listOf(
        album("1", "Northern Lights", "The Harbour Lamps"),
        album("2", "Paper Satellites", "Mira Holt"),
        album("3", "Quiet Machines", "Assembly Line"),
    )

    val tracks = listOf("Low Tide", "Salt and Glass", "Lanterns", "Night Ferry", "Breakwater")
        .mapIndexed { i, title -> TrackInfo("t$i", title, i + 1, 1, 180_000, "/x/$i.flac", artist = "The Harbour Lamps") }

    val playlists = listOf(
        PlaylistInfo("p1", "Favourites", trackCount = 12, playableCount = 5),
        PlaylistInfo("p2", "Running", trackCount = 8, playableCount = 1),
    )

    val albumGroups = listOf(
        NamedInfo("g1", "Evenings", albumCount = 3),
        NamedInfo("g2", "On the road", albumCount = 1),
    )

    private fun album(uid: String, title: String, artist: String) =
        AlbumInfo(uid, title, artist, 2024, isSingle = false, hasArt = false, folder = null)
}
