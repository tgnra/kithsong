package ng.kithso.wear.ui

import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.hapticfeedback.HapticFeedback
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.wear.compose.foundation.lazy.TransformingLazyColumn
import androidx.wear.compose.foundation.lazy.TransformingLazyColumnItemScope
import androidx.wear.compose.foundation.lazy.TransformingLazyColumnScope
import androidx.wear.compose.foundation.lazy.rememberTransformingLazyColumnState
import androidx.wear.compose.material3.ScreenScaffold
import androidx.wear.compose.material3.SurfaceTransformation
import androidx.wear.compose.material3.lazy.TransformationSpec
import androidx.wear.compose.material3.lazy.rememberTransformationSpec
import androidx.wear.compose.material3.lazy.transformedHeight

/**
 * A scrolling list screen. Items shrink and fade at the top and bottom edges of a round
 * screen, and the crown scrolls the list.
 */
@Composable
fun ListScreen(content: TransformingLazyColumnScope.(ItemTransform) -> Unit) {
    val listState = rememberTransformingLazyColumnState()
    val transform = ItemTransform(rememberTransformationSpec())
    ScreenScaffold(scrollState = listState) { contentPadding ->
        TransformingLazyColumn(state = listState, contentPadding = contentPadding) {
            content(transform)
        }
    }
}

/** Applies a [ListScreen]'s edge transformation to one of its items. */
class ItemTransform(private val spec: TransformationSpec) {
    fun surface(scope: TransformingLazyColumnItemScope): SurfaceTransformation =
        scope.SurfaceTransformation(spec)

    fun modifier(scope: TransformingLazyColumnItemScope): Modifier =
        Modifier.transformedHeight(scope, spec)
}

/**
 * The tick for a long press that opened a menu. The Wear buttons don't make one themselves,
 * and without it nothing says the press was long enough until the menu appears.
 */
fun HapticFeedback.longPressed() = performHapticFeedback(HapticFeedbackType.LongPress)
