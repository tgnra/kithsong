package ng.kithso.android.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.ui.AbsoluteAlignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.Dp
import ng.kithso.android.R

/**
 * The Kithsong wordmark: the icon's tile, then the name (R.drawable.ic_wordmark, which has a
 * night version for the dark theme, as the theme follows the system). [height] includes the
 * tile's own margin, a tenth of it above and below. [description] is for screen readers, or
 * null where the name is said nearby.
 */
@Composable
fun Wordmark(height: Dp, description: String?, modifier: Modifier = Modifier) {
    Image(
        painterResource(R.drawable.ic_wordmark),
        contentDescription = description,
        modifier = modifier.height(height).aspectRatio(352f / 100f),
    )
}

/** The wordmark's tile alone, [size] square, for where the name won't fit. */
@Composable
fun WordmarkTile(size: Dp, description: String?, modifier: Modifier = Modifier) {
    Image(
        painterResource(R.drawable.ic_wordmark),
        contentDescription = description,
        contentScale = ContentScale.FillHeight,
        alignment = AbsoluteAlignment.CenterLeft,
        modifier = modifier.size(size),
    )
}
