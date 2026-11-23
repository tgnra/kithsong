package ng.kithso.android.cast

import android.content.Context
import android.content.res.Configuration
import android.graphics.Bitmap
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.text.TextUtils
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.core.content.ContextCompat
import ng.kithso.android.R
import ng.kithso.android.data.Track
import ng.kithso.android.ui.art.loadArtBitmap
import ng.kithso.android.ui.theme.ArtColor
import ng.kithso.android.ui.theme.ArtColors
import ng.kithso.android.ui.theme.glowColors
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlin.math.max
import kotlin.math.roundToInt

/** What the picture is of a track: [card], over glows of [colors] (ARGB, one for each corner). */
class CastPicture(val card: Bitmap, val colors: IntArray)

/**
 * The picture for [track], or for none, at [width] by [height] (16:9): its art, with its title,
 * artist and album beside it, and the Kithsong wordmark at the top right, all on clear. It's in the
 * dark theme's colours, whatever the phone's, as it's on a TV.
 */
suspend fun castPicture(context: Context, track: Track?, width: Int, height: Int): CastPicture {
    val scale = height / 1080f
    val art = track?.let { loadArtBitmap(context, it.art, (ART * scale).roundToInt()) }
    val found = track?.let { ArtColors.find(context, it.art) }.orEmpty()
    return withContext(Dispatchers.Default) {
        // Art without a colour of its own still has glows to move: grey ones.
        val colors = glowColors(found.ifEmpty { listOf(ArtColor(GREY, 1f)) }, dark = true)
        CastPicture(drawCard(context, track, art, width, height, scale), IntArray(colors.size) { colors[it].toArgb() })
    }
}

private fun drawCard(context: Context, track: Track?, art: Bitmap?, width: Int, height: Int, scale: Float): Bitmap {
    val card = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
    val canvas = Canvas(card)
    canvas.scale(scale, scale)
    val w = width / scale
    drawWordmark(context, canvas, w)
    if (track == null) return card
    val top = (1080f - ART) / 2
    if (art != null) {
        val rect = RectF(ART_LEFT, top, ART_LEFT + ART, top + ART)
        // Cropped to the square, as the app does.
        val fit = max(ART / art.width, ART / art.height)
        val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
        paint.shader = BitmapShader(art, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP).apply {
            setLocalMatrix(Matrix().apply {
                setScale(fit, fit)
                postTranslate(rect.centerX() - art.width * fit / 2, rect.centerY() - art.height * fit / 2)
            })
        }
        canvas.drawRoundRect(rect, CORNER, CORNER, paint)
    }
    val left = ART_LEFT + ART + GAP
    val room = (w - left - MARGIN).roundToInt()
    val lines = listOf(
        Triple(track.title, text(72f, TEXT, bold = true), 3),
        Triple(track.artist, text(46f, TEXT, bold = false), 2),
        Triple(track.album, text(40f, TEXT_QUIET, bold = false), 2),
    ).filter { it.first.isNotBlank() }.map { (string, paint, most) -> layout(string, paint, room, most) }
    val total = lines.sumOf { it.height } + LINE_GAP * (lines.size - 1)
    var y = (1080f - total) / 2
    for (line in lines) {
        canvas.save()
        canvas.translate(left, y)
        line.draw(canvas)
        canvas.restore()
        y += line.height + LINE_GAP
    }
    return card
}

// The dark theme's, whose name is light.
private fun drawWordmark(context: Context, canvas: Canvas, w: Float) {
    val config = Configuration(context.resources.configuration).apply {
        uiMode = uiMode and Configuration.UI_MODE_NIGHT_MASK.inv() or Configuration.UI_MODE_NIGHT_YES
    }
    val mark = ContextCompat.getDrawable(context.createConfigurationContext(config), R.drawable.ic_wordmark) ?: return
    val markWidth = WORDMARK * 352 / 100
    val x = (w - MARGIN - markWidth).roundToInt()
    mark.setBounds(x, WORDMARK_TOP, x + markWidth, WORDMARK_TOP + WORDMARK)
    mark.draw(canvas)
}

private fun text(size: Float, argb: Int, bold: Boolean) = TextPaint(Paint.ANTI_ALIAS_FLAG or Paint.SUBPIXEL_TEXT_FLAG).apply {
    textSize = size
    color = argb
    typeface = if (bold) Typeface.create("sans-serif-medium", Typeface.NORMAL) else Typeface.SANS_SERIF
}

private fun layout(string: String, paint: TextPaint, width: Int, lines: Int): StaticLayout =
    StaticLayout.Builder.obtain(string, 0, string.length, paint, width)
        .setAlignment(Layout.Alignment.ALIGN_NORMAL)
        .setMaxLines(lines)
        .setEllipsize(TextUtils.TruncateAt.END)
        .setIncludePad(false)
        .build()

// For a picture 1080 high. The margins are a little more than the edge a TV may cut off.
private const val ART = 600f
private const val ART_LEFT = 140f
private const val CORNER = 28f
private const val GAP = 88f
private const val MARGIN = 96f
private const val LINE_GAP = 18f
private const val WORDMARK = 84
private const val WORDMARK_TOP = 56

private const val TEXT = 0xfff4eff4.toInt()
private const val TEXT_QUIET = 0xffcac4d0.toInt()
private val GREY = Color(0xff8c8c8c)
