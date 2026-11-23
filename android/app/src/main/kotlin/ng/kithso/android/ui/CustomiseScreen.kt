package ng.kithso.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Checkbox
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.android.R

/**
 * The phone's Customise page, from the top of Settings: the main screen's sections, to drag into the
 * order it shows them in. [onOrder] has each new order, once a drag is let go or a section is
 * moved by an accessibility action. A drag is felt as it starts, at each place it passes and as
 * it's let go. Under Artists is whether those only credited on others'
 * tracks are folded away ([foldCredited]), which [onFoldCredited] changes; only while there are
 * enough of them to fold ([canFoldCredited], see [foldsCreditedArtists]).
 */
@Composable
fun CustomiseScreen(
    order: List<Section>,
    onOrder: (List<Section>) -> Unit,
    canFoldCredited: Boolean,
    foldCredited: Boolean,
    onFoldCredited: (Boolean) -> Unit,
    bottomPadding: Dp,
    onBack: () -> Unit,
) {
    // The order as dragged, ahead of [order] until the drag's let go.
    var dragging by remember { mutableStateOf<List<Section>?>(null) }
    val sections = dragging ?: order
    val listState = rememberLazyListState()
    val reorder = remember(listState) { ReorderState(listState) }
    val haptics = LocalHapticFeedback.current
    reorder.isMovable = { it is Section }
    reorder.onStart = { haptics.performHapticFeedback(HapticFeedbackType.GestureThresholdActivate) }
    reorder.onMove = { from, to ->
        val now = dragging ?: order
        val i = now.indexOf(from)
        val j = now.indexOf(to)
        if (i >= 0 && j >= 0) {
            dragging = now.toMutableList().apply { add(j, removeAt(i)) }
            haptics.performHapticFeedback(HapticFeedbackType.SegmentTick)
        }
    }
    reorder.onDrop = {
        haptics.performHapticFeedback(HapticFeedbackType.GestureEnd)
        dragging?.let { if (it != order) onOrder(it) }
        dragging = null
    }
    val moveBy: (Int, Int) -> Unit = { i, by ->
        if (i + by in order.indices) onOrder(order.toMutableList().apply { add(i + by, removeAt(i)) })
    }

    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            state = listState,
            contentPadding = pageSides()
                .add(WindowInsets(bottom = bottomPadding))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "header") { ScreenHeader(stringResource(R.string.customise), onBack) }
            item(key = "heading") {
                SettingsHeading(stringResource(R.string.customise_sections))
                Text(
                    stringResource(R.string.customise_sections_text),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(horizontal = 16.dp).padding(bottom = 8.dp),
                )
            }
            itemsIndexed(sections, key = { _, section -> section }) { i, section ->
                SectionRow(
                    section,
                    onMoveUp = if (i > 0) ({ moveBy(i, -1) }) else null,
                    onMoveDown = if (i < sections.lastIndex) ({ moveBy(i, 1) }) else null,
                    handle = with(reorder) { Modifier.dragHandle(section) },
                    lifted = section == reorder.dragged,
                    modifier = if (section == reorder.dragged) with(reorder) { Modifier.dragged(section) } else Modifier.animateItem(),
                    option = if (section == Section.Artists && canFoldCredited) ({ FoldCreditedOption(foldCredited, onFoldCredited) }) else null,
                )
            }
        }
    }
}

/**
 * A section to drag by its handle, as the queue's rows are; it's lifted while dragged. Its
 * [option], if any, goes under it and moves with it.
 */
@Composable
private fun SectionRow(
    section: Section,
    onMoveUp: (() -> Unit)?,
    onMoveDown: (() -> Unit)?,
    handle: Modifier,
    lifted: Boolean,
    modifier: Modifier,
    option: (@Composable () -> Unit)? = null,
) {
    val moveUp = stringResource(R.string.move_up)
    val moveDown = stringResource(R.string.move_down)
    Column(
        modifier
            .fillMaxWidth()
            .padding(horizontal = 8.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(if (lifted) MaterialTheme.colorScheme.surfaceContainerHigh else Color.Transparent),
    ) {
        Row(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 56.dp)
                .semantics(mergeDescendants = true) {
                    customActions = listOfNotNull(
                        onMoveUp?.let { move -> CustomAccessibilityAction(moveUp) { move(); true } },
                        onMoveDown?.let { move -> CustomAccessibilityAction(moveDown) { move(); true } },
                    )
                },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            // The row says what can be done with it, so the handle needn't be a stop of its own.
            Box(handle.size(48.dp), contentAlignment = Alignment.Center) {
                Icon(Icons.DragHandle, contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
            }
            Text(stringResource(section.title), style = MaterialTheme.typography.bodyLarge, modifier = Modifier.padding(start = 8.dp))
        }
        option?.invoke()
    }
}

/** The check under Artists: the whole row toggles it, its box under the section's name. */
@Composable
private fun FoldCreditedOption(checked: Boolean, onChange: (Boolean) -> Unit) {
    val haptics = LocalHapticFeedback.current
    Row(
        Modifier
            .fillMaxWidth()
            .toggleable(checked, role = Role.Checkbox) {
                haptics.performHapticFeedback(if (it) HapticFeedbackType.ToggleOn else HapticFeedbackType.ToggleOff)
                onChange(it)
            }
            .padding(start = 54.dp, end = 16.dp, top = 4.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Checkbox(checked, onCheckedChange = null)
        Text(
            stringResource(R.string.customise_fold_credited),
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.padding(start = 12.dp),
        )
    }
}
