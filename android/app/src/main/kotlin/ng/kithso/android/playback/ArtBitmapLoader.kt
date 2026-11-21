package ng.kithso.android.playback

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.annotation.OptIn
import androidx.media3.common.MediaMetadata
import androidx.media3.common.util.BitmapLoader
import androidx.media3.common.util.UnstableApi
import ng.kithso.android.ui.art.loadArtBitmap
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import com.google.common.util.concurrent.SettableFuture
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import java.io.IOException

/**
 * Art for the notification and lock screen, from a queue item's [artRef] through the app's
 * image loader, so covers come out as they do in the app, and from its caches. Browse items
 * carry only an [ArtProvider] artwork URI, for Android Auto, and have their art loaded from what
 * the URI says, the same way.
 */
@OptIn(UnstableApi::class)
class ArtBitmapLoader(private val context: Context, private val scope: CoroutineScope) : BitmapLoader {
    private val artPx = (ART_DP * context.resources.displayMetrics.density).toInt()

    override fun supportsMimeType(mimeType: String) = mimeType.startsWith("image/")

    override fun decodeBitmap(data: ByteArray): ListenableFuture<Bitmap> = load {
        BitmapFactory.decodeByteArray(data, 0, data.size)
    }

    override fun loadBitmap(uri: Uri): ListenableFuture<Bitmap> {
        val art = ArtProvider.artFor(context, uri)
            ?: return Futures.immediateFailedFuture(UnsupportedOperationException("Art comes from MediaMetadata.artRef"))
        return load { loadArtBitmap(context, art, artPx) }
    }

    override fun loadBitmapFromMetadata(metadata: MediaMetadata): ListenableFuture<Bitmap>? {
        val art = metadata.artRef ?: metadata.artworkUri?.let { ArtProvider.artFor(context, it) } ?: return null
        return load { loadArtBitmap(context, art, artPx) }
    }

    private fun load(block: suspend () -> Bitmap?): ListenableFuture<Bitmap> {
        val future = SettableFuture.create<Bitmap>()
        scope.launch(Dispatchers.IO) {
            try {
                val bitmap = block()
                if (bitmap != null) future.set(bitmap) else future.setException(IOException("No art"))
            } catch (e: Exception) {
                future.setException(e)
            }
        }
        return future
    }

    private companion object {
        // The largest the system keeps of a media session's art (config_mediaMetadataBitmapMaxSize):
        // it scales down anything bigger, and the media controls draw it across the screen.
        const val ART_DP = 320
    }
}
