package app.musicapp.playback

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.annotation.OptIn
import androidx.media3.common.MediaMetadata
import androidx.media3.common.util.BitmapLoader
import androidx.media3.common.util.UnstableApi
import app.musicapp.ui.art.loadArtBitmap
import com.google.common.util.concurrent.Futures
import com.google.common.util.concurrent.ListenableFuture
import com.google.common.util.concurrent.SettableFuture
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import java.io.IOException

/**
 * Art for the notification and lock screen, from a queue item's [artRef] through the app's
 * image loader, so covers and embedded art come out as they do in the app, and from its caches.
 * Items carry no artwork URI: the system couldn't read files in app storage anyway.
 */
@OptIn(UnstableApi::class)
class ArtBitmapLoader(private val context: Context, private val scope: CoroutineScope) : BitmapLoader {
    override fun supportsMimeType(mimeType: String) = mimeType.startsWith("image/")

    override fun decodeBitmap(data: ByteArray): ListenableFuture<Bitmap> = load {
        BitmapFactory.decodeByteArray(data, 0, data.size)
    }

    override fun loadBitmap(uri: Uri): ListenableFuture<Bitmap> =
        Futures.immediateFailedFuture(UnsupportedOperationException("Art comes from MediaMetadata.artRef"))

    override fun loadBitmapFromMetadata(metadata: MediaMetadata): ListenableFuture<Bitmap>? {
        val art = metadata.artRef ?: return null
        return load { loadArtBitmap(context, art, ART_PX) }
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
        // What the notification and lock screen show it at, near enough.
        const val ART_PX = 512
    }
}
