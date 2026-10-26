package app.musicapp.wear.ui

import android.graphics.BitmapFactory
import android.util.LruCache
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.wear.compose.material3.Text
import app.musicapp.AlbumInfo
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.math.absoluteValue

// Muted, so white text reads on all of them.
private val placeholderColors = listOf(
    Color(0xFF5C6BC0), Color(0xFF26A69A), Color(0xFFAB47BC), Color(0xFFEF6C00),
    Color(0xFF8D6E63), Color(0xFF42A5F5), Color(0xFFEC407A), Color(0xFF7CB342),
)

// Decoded covers by path and size in pixels; small, since covers are decoded at list size.
private val covers = LruCache<String, ImageBitmap>(32)

/**
 * The cover of [album]: the cover.jpg or cover.png in its folder, or, when it has none, a
 * coloured square with the album's first letter, coloured by the title so it stays the same.
 */
@Composable
fun AlbumArt(album: AlbumInfo, size: Dp, modifier: Modifier = Modifier) {
    val px = with(LocalDensity.current) { size.roundToPx() }
    val cover by produceState<ImageBitmap?>(null, album.folder, px) {
        value = album.folder?.takeIf { album.hasArt }?.let { loadCover(it, px) }
    }
    val shape = RoundedCornerShape(size / 6)
    val image = cover
    if (image != null) {
        Image(
            image,
            contentDescription = null,
            modifier = modifier.size(size).clip(shape),
            contentScale = ContentScale.Crop,
        )
        return
    }
    val color = placeholderColors[album.title.hashCode().absoluteValue % placeholderColors.size]
    Box(
        modifier = modifier.size(size).clip(shape).background(color),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = album.title.take(1).uppercase(),
            color = Color.White,
            fontSize = (size.value * 0.45f).sp,
        )
    }
}

/** Decodes the cover in [folder], subsampled to no less than [px] square. */
private suspend fun loadCover(folder: String, px: Int): ImageBitmap? = withContext(Dispatchers.IO) {
    val file = listOf("cover.jpg", "cover.png").map { File(folder, it) }.firstOrNull { it.isFile }
        ?: return@withContext null
    val key = "${file.path}@$px"
    covers.get(key)?.let { return@withContext it }

    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeFile(file.path, bounds)
    var sample = 1
    while (bounds.outWidth / (sample * 2) >= px && bounds.outHeight / (sample * 2) >= px) sample *= 2
    val bitmap = BitmapFactory.decodeFile(file.path, BitmapFactory.Options().apply { inSampleSize = sample })
        ?: return@withContext null
    bitmap.asImageBitmap().also { covers.put(key, it) }
}

val ListArtSize = 36.dp
