package ng.kithso.android.ui.art

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Shader
import android.graphics.Typeface
import androidx.compose.foundation.background
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import ng.kithso.android.data.ArtRef
import coil3.ImageLoader
import coil3.SingletonImageLoader
import coil3.asImage
import coil3.compose.AsyncImage
import coil3.decode.DataSource
import coil3.decode.ImageSource
import coil3.disk.DiskCache
import coil3.disk.directory
import coil3.fetch.FetchResult
import coil3.fetch.Fetcher
import coil3.fetch.ImageFetchResult
import coil3.fetch.SourceFetchResult
import coil3.key.Keyer
import coil3.memory.MemoryCache
import coil3.request.ImageRequest
import coil3.request.SuccessResult
import coil3.request.allowHardware
import coil3.request.Options
import coil3.size.Precision
import coil3.size.pxOrElse
import coil3.toBitmap
import kotlinx.coroutines.flow.distinctUntilChanged
import java.io.File
import java.io.IOException
import kotlin.math.absoluteValue

/**
 * Where cover images come from. Coil does the caching and threading; a source only decodes.
 * [FileArt] decodes cover files, and falls back to [PlaceholderArt].
 */
fun interface ArtSource {
    /** [art]'s cover, no smaller than [px] square where possible, or null for none. */
    suspend fun load(art: ArtRef, px: Int): Bitmap?
}

/**
 * [ArtRef.file]: a cover image, decoded at the size asked for rather than at full size.
 * Without a file, or when it won't decode, the art comes from [fallback].
 */
class FileArt(private val fallback: ArtSource = PlaceholderArt) : ArtSource {
    override suspend fun load(art: ArtRef, px: Int): Bitmap? {
        val file = art.file ?: return fallback.load(art, px)
        val bitmap = decodeSampled(px) { options -> BitmapFactory.decodeFile(file, options) }
        return bitmap ?: fallback.load(art, px)
    }

    /**
     * Decodes with the largest power-of-two subsampling that keeps the shorter side at least
     * [px], then scales the rest of the way down while decoding.
     */
    private fun decodeSampled(px: Int, decode: (BitmapFactory.Options) -> Bitmap?): Bitmap? {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        decode(bounds)
        val side = minOf(bounds.outWidth, bounds.outHeight)
        if (side <= 0) return null
        var sample = 1
        while (side / (sample * 2) >= px) sample *= 2
        return decode(BitmapFactory.Options().apply {
            inSampleSize = sample
            val sampled = side / sample
            if (sampled > px) {
                inScaled = true
                inDensity = sampled
                inTargetDensity = px
            }
        })
    }
}

/** A coloured square with the first letter of the label, coloured by the label so it stays the same. */
object PlaceholderArt : ArtSource {
    // Muted, so white text reads on all of them (the same as the watch's).
    private val colors = intArrayOf(
        0xFF5C6BC0.toInt(), 0xFF26A69A.toInt(), 0xFFAB47BC.toInt(), 0xFFEF6C00.toInt(),
        0xFF8D6E63.toInt(), 0xFF42A5F5.toInt(), 0xFFEC407A.toInt(), 0xFF7CB342.toInt(),
    )

    override suspend fun load(art: ArtRef, px: Int): Bitmap {
        val size = px.coerceIn(32, 1024)
        val hash = art.label.hashCode().absoluteValue
        val from = colors[hash % colors.size]
        val to = colors[(hash / colors.size) % colors.size]
        val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        val s = size.toFloat()
        canvas.drawRect(0f, 0f, s, s, Paint().apply {
            shader = LinearGradient(0f, 0f, s, s, from, to, Shader.TileMode.CLAMP)
        })
        val text = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = 0xE6FFFFFF.toInt()
            textSize = s * 0.42f
            textAlign = Paint.Align.CENTER
            typeface = Typeface.DEFAULT_BOLD
        }
        val y = s / 2 - (text.descent() + text.ascent()) / 2
        canvas.drawText(art.label.take(1).uppercase(), s / 2, y, text)
        return bitmap
    }
}

/**
 * Builds the app's image loader, which loads [ArtRef]s from [source]. Decoded art is kept in
 * memory, and art decoded from a file is also kept on disk at the size it was decoded at, so
 * a cover that has left the memory cache decodes again quickly.
 */
fun artImageLoader(context: Context, source: ArtSource): ImageLoader =
    ImageLoader.Builder(context)
        .components {
            add(ArtKeyer())
            add(ArtFetcher.Factory(source))
        }
        .memoryCache { MemoryCache.Builder().maxSizePercent(context, 0.25).build() }
        .diskCache {
            DiskCache.Builder().directory(File(context.cacheDir, "art")).maxSizeBytes(128L * 1024 * 1024).build()
        }
        .build()

// By file when there is one, so art shared by an album and its artist is cached once, and
// with its version, so a changed cover is loaded afresh.
private fun ArtRef.key() = if (file != null) "art:$file:$version" else "art:$uid"

private class ArtKeyer : Keyer<ArtRef> {
    override fun key(data: ArtRef, options: Options) = data.key()
}

private class ArtFetcher(
    private val art: ArtRef,
    private val options: Options,
    private val source: ArtSource,
    private val diskCache: DiskCache?,
) : Fetcher {
    override suspend fun fetch(): FetchResult? {
        val requested = maxOf(options.size.width.pxOrElse { DEFAULT_PX }, options.size.height.pxOrElse { DEFAULT_PX })
        // Rounded up, so a cell a pixel off the size it was prefetched at is served the same art.
        val px = (requested + SIZE_STEP - 1) / SIZE_STEP * SIZE_STEP
        // Placeholders draw faster than they'd read back, so only art from files goes on disk.
        val cache = diskCache?.takeIf { art.file != null }
        val diskKey = "${art.key()}@$px"
        cache?.openSnapshot(diskKey)?.let { snapshot ->
            return SourceFetchResult(
                ImageSource(snapshot.data, cache.fileSystem, diskKey, snapshot),
                mimeType = "image/jpeg",
                dataSource = DataSource.DISK,
            )
        }

        val bitmap = source.load(art, px) ?: return null
        cache?.openEditor(diskKey)?.let { editor ->
            try {
                cache.fileSystem.write(editor.data) { bitmap.compress(Bitmap.CompressFormat.JPEG, 90, outputStream()) }
                editor.commit()
            } catch (e: IOException) {
                editor.abort()
            }
        }
        // Smaller than asked for only when the original is, and then it's the whole image.
        return ImageFetchResult(bitmap.asImage(), isSampled = bitmap.width >= px, dataSource = DataSource.DISK)
    }

    class Factory(private val source: ArtSource) : Fetcher.Factory<ArtRef> {
        override fun create(data: ArtRef, options: Options, imageLoader: ImageLoader): Fetcher =
            ArtFetcher(data, options, source, imageLoader.diskCache)
    }

    private companion object {
        const val DEFAULT_PX = 512
        const val SIZE_STEP = 64
    }
}

/**
 * [art] as a software bitmap about [px] square, through the app's image loader and its caches,
 * for code that needs the pixels: the media notification, and colours taken from the art.
 */
suspend fun loadArtBitmap(context: Context, art: ArtRef, px: Int): Bitmap? {
    val request = ImageRequest.Builder(context).data(art).size(px).precision(Precision.INEXACT).allowHardware(false).build()
    return (SingletonImageLoader.get(context).execute(request) as? SuccessResult)?.image?.toBitmap()
}

/** [art] cropped to [shape], on a neutral background while it loads. */
@Composable
fun AlbumArt(art: ArtRef, contentDescription: String?, shape: Shape, modifier: Modifier = Modifier) {
    AsyncImage(
        model = art,
        contentDescription = contentDescription,
        contentScale = ContentScale.Crop,
        modifier = modifier.clip(shape).background(MaterialTheme.colorScheme.surfaceVariant),
    )
}

/**
 * Loads the art of the [ahead] list items past the last visible one, and the items before the
 * first, so covers are in the memory cache before they scroll into view. [artAt] gives the art
 * of a list item, and [px] the size to load it at: the largest size it's shown at, so the
 * cached image serves the smaller ones too.
 */
@Composable
fun PrefetchArt(state: LazyListState, px: Int, ahead: Int = 6, artAt: (Int) -> List<ArtRef>) {
    val context = LocalContext.current
    val currentArtAt by rememberUpdatedState(artAt)
    LaunchedEffect(state, px) {
        val loader = SingletonImageLoader.get(context)
        snapshotFlow {
            val visible = state.layoutInfo.visibleItemsInfo
            if (visible.isEmpty()) IntRange.EMPTY else visible.first().index..visible.last().index
        }
            .distinctUntilChanged()
            .collect { visible ->
                if (visible.isEmpty()) return@collect
                val around = (visible.first - ahead / 2 until visible.first) +
                    (visible.last + 1..visible.last + ahead)
                for (index in around) {
                    for (art in currentArtAt(index)) {
                        // Inexact, as AsyncImage's requests are, so a larger cached image will do.
                        loader.enqueue(
                            ImageRequest.Builder(context).data(art).size(px).precision(Precision.INEXACT).build(),
                        )
                    }
                }
            }
    }
}
