package app.kithsong.wear.ui

import app.kithsong.AlbumInfo
import app.kithsong.TrackInfo

/** Made-up albums for the @Previews. */
internal object PreviewData {
    val albums = listOf(
        album("1", "Northern Lights", "The Harbour Lamps"),
        album("2", "Paper Satellites", "Mira Holt"),
        album("3", "Quiet Machines", "Assembly Line"),
    )

    val tracks = listOf("Low Tide", "Salt and Glass", "Lanterns", "Night Ferry", "Breakwater")
        .mapIndexed { i, title -> TrackInfo("t$i", title, i + 1, 1, 180_000, "/x/$i.flac") }

    private fun album(uid: String, title: String, artist: String) =
        AlbumInfo(uid, title, artist, 2024, isSingles = false, hasArt = false, folder = null)
}
