package ng.kithso.android.ui

import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
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

/**
 * The wordmark's tile alone (R.drawable.ic_logo). At the same [height] it's the size the
 * [Wordmark] draws it, margin and all.
 */
@Composable
fun Logo(height: Dp, description: String?, modifier: Modifier = Modifier) {
    Image(
        painterResource(R.drawable.ic_logo),
        contentDescription = description,
        modifier = modifier.height(height).aspectRatio(1f),
    )
}

/**
 * The wordmark's name alone (R.drawable.ic_name, with a night version), cropped to its ink, so
 * [height] is the letters' own, from the h's top to the g's tail.
 */
@Composable
fun WordmarkName(height: Dp, description: String?, modifier: Modifier = Modifier) {
    Image(
        painterResource(R.drawable.ic_name),
        contentDescription = description,
        modifier = modifier.height(height).aspectRatio(236f / 74f),
    )
}
