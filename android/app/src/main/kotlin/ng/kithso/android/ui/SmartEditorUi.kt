package ng.kithso.android.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.relocation.BringIntoViewRequester
import androidx.compose.foundation.relocation.bringIntoViewRequester
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.PlaylistChoice
import ng.kithso.android.data.SmartCondition
import ng.kithso.android.data.SmartGroup
import ng.kithso.android.data.SmartListEditor
import ng.kithso.android.data.SmartMode
import ng.kithso.android.data.SmartNode
import ng.kithso.android.data.SmartPreview
import ng.kithso.android.data.SmartProblem
import ng.kithso.android.data.SmartRule
import ng.kithso.android.data.SmartUnit
import ng.kithso.core.CoreSmart
import ng.kithso.core.SmartField
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import java.text.DateFormat
import java.util.Calendar
import java.util.Date
import java.util.TimeZone

/** What the smart list editor opens on: a new smart playlist (or album group, [isAlbums]), or smart list [uid]. */
@Immutable
sealed interface SmartEditTarget {
    data class New(val isAlbums: Boolean) : SmartEditTarget
    data class Edit(val uid: String) : SmartEditTarget
}

private const val PREVIEW_DELAY_MS = 250L
private const val PREVIEW_ROWS = 50
private const val PROBLEM_ROWS = 4
private const val MAX_NAME = 255
private const val MAX_LIMIT = 100_000

/**
 * The smart list editor's state: the rule being edited ([rule], once read in or a ready-made
 * one picked), the list's [name], the groups opened over the list's page ([pages], by id), and
 * what's been worked out about the rule as it stands: each group's sentence ([sentences]), each
 * row's count ([counts], with the rule it was counted from), the shown page's [preview] and
 * the list's [total], all by [SmartNode.id].
 */
@Stable
internal class SmartEditorState(val editor: SmartListEditor, val target: SmartEditTarget) {
    val creating = target is SmartEditTarget.New
    var isAlbums by mutableStateOf((target as? SmartEditTarget.New)?.isAlbums == true)
    var uid: String? = (target as? SmartEditTarget.Edit)?.uid
    var loaded by mutableStateOf(false)
    var rule by mutableStateOf<SmartRule?>(null)
    var readOnly by mutableStateOf(false)
    var name by mutableStateOf("")
    var limitText by mutableStateOf("")
    var oldName: String? = null
    var oldRule: String? = null
    var fields: List<SmartField> = emptyList()
    var playlists: List<PlaylistChoice> = emptyList()
    var groups: List<PlaylistChoice> = emptyList()

    var stack by mutableStateOf(BackStack<Int>())
    val pages: List<Int> get() = stack.pages
    val sentences = mutableStateMapOf<Int, String>()
    val counts = mutableStateMapOf<Int, Pair<String, Int>>()
    var preview by mutableStateOf<Pair<Int, SmartPreview?>?>(null)
    var total by mutableIntStateOf(-1)
    var timed by mutableIntStateOf(-1) // what a limit by duration keeps; -1 if there's none
    var counted by mutableStateOf(false)
    var saving by mutableStateOf(false)

    /** A condition to bring into view and focus once its page shows, after Show on a problem. */
    var focus by mutableStateOf<Int?>(null)

    /** Fills the editor from [ruleJson], or makes it read-only if the editor can't show it. */
    fun start(name: String, ruleJson: String) {
        val r = newRule()
        val ok = CoreSmart.isValid(isAlbums, ruleJson) && r.load(ruleJson)
        val shown = if (ok) r else newRule()
        this.name = name
        readOnly = !ok
        oldRule = if (creating) null else if (!ok) ruleJson else shown.ruleJson() // as written here
        if (creating && shown.root.kids.isEmpty()) shown.addCondition(shown.root) // a blank rule: one to fill in
        limitText = shown.limit.toString()
        rule = shown
    }

    private fun newRule() = SmartRule(isAlbums, fields, { CoreSmart.orderLabel(it, false) != null }, playlists, groups)

    /** The group whose page is shown. */
    fun visibleGroup(r: SmartRule): SmartGroup = pages.lastOrNull()?.let { r.find(it) as? SmartGroup } ?: r.root

    /** Whether Save can go: a name, no problems, and a limit that's a number. */
    fun canSave(r: SmartRule): Boolean =
        !readOnly && !saving && name.isNotBlank() && r.problems().isEmpty() && (!r.limited || limitValue() != null)

    fun limitValue(): Int? = limitText.trim().toIntOrNull()?.takeIf { it in 1..MAX_LIMIT }

    /** How many of [n] picks the limit keeps. */
    fun keptByLimit(r: SmartRule, n: Int): Int = when {
        !r.limited -> n
        r.limitUnit == SmartRule.LIMIT_ITEMS -> minOf(n, r.limit)
        timed >= 0 -> minOf(n, timed)
        else -> n
    }
}

/**
 * The words of the rule's pickers, from the core (CoreSmart) so every platform's editor reads
 * alike: each field's label, each field's operators', and the sort orders (a [SmartRule.sorts]
 * key each, both ways for every sortable field). A date's empty and not_empty read as never
 * and any time, "Last played is never", as on Linux.
 */
@Immutable
private class SmartWords(val fields: List<String>, val ops: List<List<String>>, val orders: List<Pair<Int, String>>)

@Composable
private fun rememberSmartWords(rule: SmartRule): SmartWords {
    val never = stringResource(R.string.smart_op_never)
    val ever = stringResource(R.string.smart_op_ever)
    return remember(rule, never, ever) {
        val fields = rule.fields.map { CoreSmart.fieldLabel(it.name) ?: it.name }
        val ops = rule.fields.map { f ->
            f.ops.map { op ->
                when {
                    f.type == "date" && op == "empty" -> never
                    f.type == "date" && op == "not_empty" -> ever
                    else -> CoreSmart.opLabel(f.type, op) ?: op
                }
            }
        }
        val orders = rule.sortable.flatMapIndexed { s, field ->
            listOf(false, true).mapNotNull { desc ->
                CoreSmart.orderLabel(rule.fields[field].name, desc)?.let { (s * 2 + if (desc) 1 else 0) to it }
            }
        }
        SmartWords(fields, ops, orders)
    }
}

/**
 * The smart list editor, ported from Linux's (smart_editor.c), over everything as a layer of its
 * own: the list's page, then a page for each group opened, each sliding in over the one before,
 * so back (or swiping a group's page away) goes up a group, and on the list's page closes the
 * editor without saving. A new list starts with a choice of ready-made rules, or a blank one.
 *
 * A page has the name (the list's, or a group's, whose placeholder is its sentence), how its
 * conditions combine (all, any or none of them), a row for each condition (field, operator and
 * a value that fits its type) or group (its name or sentence; tapping it opens its page), Add
 * condition and Add group, and on the list's page its order and limit. Every row shows how many
 * tracks or albums it matches on its own, and the page a preview of what the list holds or what
 * the group alone matches; they follow the rule a quarter of a second after the last change,
 * worked out off the main thread ([SmartListEditor.countRule] and previewRule). A value missing
 * or wrong anywhere is listed at the bottom, with Show to open its group, and Save waits until
 * there are none. A group page has the path from the list down, a tap on one going back to it.
 *
 * A rule the editor can't show (from a newer version, say) opens read-only, as its sentence,
 * and is never written back. [onSaved] gets a new list's uid once it's made, and an edited
 * one's after it's saved; [onClose] closes the editor, and [onLeaving] says it's on its way,
 * sliding out, so back is for what's beneath it.
 */
@Composable
internal fun SmartEditorScreen(
    editor: SmartListEditor,
    target: SmartEditTarget,
    onSaved: (uid: String, created: Boolean) -> Unit,
    onLeaving: () -> Unit,
    onClose: () -> Unit,
) {
    val state = remember(editor, target) { SmartEditorState(editor, target) }
    LaunchedEffect(state) {
        val choices = editor.smartChoices()
        state.playlists = choices?.first.orEmpty()
        state.groups = choices?.second.orEmpty()
        if (target is SmartEditTarget.Edit) {
            val info = editor.smartList(target.uid)
            if (info == null) {
                onClose()
                return@LaunchedEffect
            }
            state.isAlbums = info.isAlbums
            state.oldName = info.name
            state.fields = CoreSmart.fields(info.isAlbums)
            state.start(info.name, info.rule)
        } else {
            state.fields = CoreSmart.fields(state.isAlbums)
        }
        state.loaded = true
    }
    SmartWork(state)
    val rule = state.rule
    Box(Modifier.fillMaxSize()) {
        DismissibleScreen(
            DismissDirection.Right,
            onLeaving = onLeaving,
            onDismissed = onClose,
            backEnabled = state.stack.top == null,
            // Swiping the list's page away would lose the edit: only back or Cancel close it.
            draggable = false,
        ) { dismiss ->
            when {
                !state.loaded -> SmartPageFrame(state, null, null, onBack = dismiss, onSave = {}) {}
                rule == null -> PresetsPage(state, onBack = dismiss)
                else -> SmartPage(state, rule, rule.root, onBack = dismiss, onSaved = onSaved, onClose = onClose)
            }
        }
        if (rule != null) {
            for (layer in state.stack.layers) {
                val group = rule.find(layer.page) as? SmartGroup ?: continue
                key(layer.id) {
                    DismissibleScreen(
                        DismissDirection.Right,
                        onLeaving = { state.stack = state.stack.leave(layer.id) },
                        onDismissed = { state.stack = state.stack.remove(layer.id) },
                        backEnabled = layer.id == state.stack.top?.id,
                    ) { dismiss ->
                        SmartPage(state, rule, group, onBack = dismiss, onSaved = onSaved, onClose = onClose)
                    }
                }
            }
        }
    }
}

/**
 * What's worked out about the rule as it stands, a quarter of a second after the last change:
 * every group's sentence (for its row, its page's placeholder, the path and the problems), the
 * shown page's preview and the list's total, then each of its rows' counts. A newer change
 * drops what's still being worked out.
 */
@Composable
private fun SmartWork(state: SmartEditorState) {
    val rule = state.rule ?: return
    if (state.readOnly) return
    LaunchedEffect(state, rule) {
        snapshotFlow { workOf(state, rule) }.collectLatest { work ->
            state.counted = false
            delay(PREVIEW_DELAY_MS)
            for ((id, json) in work.groups) {
                val sentence = json?.let { state.editor.describeRule(rule.isAlbums, it) }
                if (sentence == null) state.sentences.remove(id) else state.sentences[id] = sentence
            }
            val preview = work.previewRule?.let { state.editor.previewRule(rule.isAlbums, it, PREVIEW_ROWS) }
            state.total = if (work.previewIsList && preview != null) preview.count
            else work.listRule?.let { state.editor.countRule(rule.isAlbums, it) } ?: -1
            state.timed = work.timedRule?.let { state.editor.countRule(rule.isAlbums, it) } ?: -1
            state.preview = work.page to preview
            state.counted = true
            for ((id, json) in work.rows) {
                if (state.counts[id]?.first == json) continue
                state.editor.countRule(rule.isAlbums, json)?.let { state.counts[id] = json to it }
            }
        }
    }
}

/**
 * The rule as [SmartWork] needs it: the list's rule without its limit, and with it if that's a
 * limit by duration, the shown [page] and what
 * it previews, its rows' rules (an empty group's isn't counted), and every group's rule for its
 * sentence (null if it says nothing yet).
 */
@Immutable
private data class Work(
    val listRule: String?,
    val timedRule: String?,
    val page: Int,
    val previewRule: String?,
    val previewIsList: Boolean,
    val rows: List<Pair<Int, String>>,
    val groups: List<Pair<Int, String?>>,
)

private fun workOf(state: SmartEditorState, rule: SmartRule): Work {
    val listRule = rule.ruleJson(withLimit = false)
    val g = state.visibleGroup(rule)
    val previewRule = when {
        g.parent == null -> listRule
        g.kids.isNotEmpty() -> rule.matchJson(g, lenient = false)
        else -> null
    }
    val rows = g.kids.mapNotNull { n ->
        if (n is SmartGroup && n.kids.isEmpty()) null else rule.matchJson(n, lenient = false)?.let { n.id to it }
    }
    val groups = ArrayList<Pair<Int, String?>>()
    fun walk(group: SmartGroup) {
        for (k in group.kids) {
            if (k !is SmartGroup) continue
            walk(k)
            groups += k.id to (if (rule.saysSomething(k)) rule.matchJson(k, lenient = true) else null)
        }
    }
    walk(rule.root)
    val timedRule = if (rule.limited && rule.limitUnit != SmartRule.LIMIT_ITEMS) rule.ruleJson() else null
    return Work(listRule, timedRule, g.id, previewRule, g.parent == null, rows, groups)
}

/** What a group is called in the path and the problems: its name, else its sentence, else "Group"; the list's own is the list's name. */
@Composable
private fun groupTitle(state: SmartEditorState, g: SmartGroup): String = when {
    g.parent == null -> state.name.trim().ifEmpty { stringResource(R.string.untitled) }
    g.isNamed -> g.name.trim()
    else -> state.sentences[g.id] ?: stringResource(R.string.smart_group_untitled)
}

/**
 * A page's frame: the top bar (back, or on the list's page Cancel, the title, and Save), the
 * path once a group is open, the page's [content] scrolling beneath, and at the bottom the
 * problems and how many the rule matches.
 */
@Composable
private fun SmartPageFrame(
    state: SmartEditorState,
    rule: SmartRule?,
    group: SmartGroup?,
    onBack: () -> Unit,
    onSave: () -> Unit,
    content: @Composable () -> Unit,
) {
    val title = when {
        group != null && group.parent != null -> groupTitle(state, group)
        rule == null && state.creating -> stringResource(R.string.smart_preset_title)
        else -> stringResource(
            when {
                state.creating -> if (state.isAlbums) R.string.smart_new_group_title else R.string.smart_new_playlist_title
                else -> if (state.isAlbums) R.string.smart_edit_group_title else R.string.smart_edit_playlist_title
            },
        )
    }
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        Column(
            Modifier
                .fillMaxSize()
                .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal))
                .imePadding(),
        ) {
            Row(Modifier.fillMaxWidth().statusBarsPadding().padding(4.dp), verticalAlignment = Alignment.CenterVertically) {
                val root = group == null || group.parent == null
                if (root) {
                    TextButton(onClick = onBack) {
                        Text(stringResource(if (state.readOnly) R.string.close else R.string.cancel))
                    }
                } else {
                    IconButton(onClick = onBack) { Icon(Icons.ArrowBack, contentDescription = stringResource(R.string.back)) }
                }
                Text(
                    title,
                    style = MaterialTheme.typography.titleLarge,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f).padding(horizontal = 8.dp).semantics { heading() },
                )
                if (rule != null && !state.readOnly) {
                    TextButton(onClick = onSave, enabled = state.canSave(rule)) {
                        Text(stringResource(if (state.creating) R.string.create else R.string.save))
                    }
                }
            }
            if (rule != null && group != null && group.parent != null) SmartPath(state, rule)
            Column(Modifier.weight(1f).fillMaxWidth().verticalScroll(rememberScrollState())) {
                Column(Modifier.widthIn(max = 720.dp).align(Alignment.CenterHorizontally).padding(horizontal = 16.dp)) {
                    content()
                    Spacer(Modifier.size(24.dp))
                }
            }
            if (rule != null && !state.readOnly) SmartBottom(state, rule)
        }
    }
}

/** The list's name, then the groups opened, each but the last a way back to it, marked where something in it needs fixing. */
@Composable
private fun SmartPath(state: SmartEditorState, rule: SmartRule) {
    val groups = listOf(rule.root) + state.pages.mapNotNull { rule.find(it) as? SmartGroup }
    val name = stringResource(R.string.smart_path_name)
    Row(
        Modifier
            .fillMaxWidth()
            .horizontalScroll(rememberScrollState())
            .padding(horizontal = 8.dp)
            .semantics { contentDescription = name },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        for ((i, g) in groups.withIndex()) {
            if (i > 0) Text("›", color = MaterialTheme.colorScheme.onSurfaceVariant)
            val bad = rule.hasProblem(g)
            val color = if (bad) MaterialTheme.colorScheme.error else Color.Unspecified
            val label = @Composable {
                if (bad) Icon(Icons.Warning, contentDescription = stringResource(R.string.smart_group_has_problem), modifier = Modifier.size(16.dp).padding(end = 2.dp))
                Text(groupTitle(state, g), maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.widthIn(max = 200.dp))
            }
            if (i < groups.lastIndex) {
                TextButton(onClick = { state.stack = state.stack.keep(i) }) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        CompositionLocalProvider(LocalContentColor provides if (bad) color else MaterialTheme.colorScheme.primary) {
                            label()
                        }
                    }
                }
            } else {
                Row(Modifier.padding(horizontal = 12.dp, vertical = 8.dp), verticalAlignment = Alignment.CenterVertically) {
                    CompositionLocalProvider(LocalContentColor provides if (bad) color else MaterialTheme.colorScheme.onSurface) {
                        label()
                    }
                }
            }
        }
    }
}

/** A list's page, or a group's: see [SmartEditorScreen]. */
@Composable
private fun SmartPage(
    state: SmartEditorState,
    rule: SmartRule,
    group: SmartGroup,
    onBack: () -> Unit,
    onSaved: (String, Boolean) -> Unit,
    onClose: () -> Unit,
) {
    val words = rememberSmartWords(rule)
    val scope = rememberCoroutineScope()
    val save: () -> Unit = save@{
        if (!state.canSave(rule)) return@save
        rule.limit = state.limitValue() ?: rule.limit
        val json = rule.ruleJson() ?: return@save
        if (!CoreSmart.isValid(rule.isAlbums, json)) return@save
        val name = state.name.trim()
        state.saving = true
        scope.launch {
            val uid = state.uid
            if (state.creating) {
                val made = state.editor.createSmart(name, rule.isAlbums, json)
                state.saving = false
                if (made != null) {
                    onClose()
                    onSaved(made, true)
                }
            } else if (uid != null) {
                val newName = name.takeIf { it != state.oldName }
                val newRule = json.takeIf { it != state.oldRule }
                val ok = newName == null && newRule == null || state.editor.saveSmart(uid, newName, newRule)
                state.saving = false
                if (ok) {
                    onClose()
                    if (newName != null || newRule != null) onSaved(uid, false)
                }
            }
        }
    }
    SmartPageFrame(state, rule, group, onBack = onBack, onSave = save) {
        val root = group.parent == null
        NameField(state, rule, group)
        if (state.readOnly) {
            ReadOnlyRule(state, rule)
            return@SmartPageFrame
        }
        ModePicker(group)
        Column(Modifier.padding(top = 8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            if (group.kids.isEmpty()) {
                Text(
                    stringResource(if (root) R.string.smart_no_conditions else R.string.smart_group_empty_hint),
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(vertical = 16.dp, horizontal = 4.dp),
                )
            }
            for (node in group.kids) {
                key(node.id) {
                    when (node) {
                        is SmartCondition -> ConditionRow(state, rule, words, node)
                        is SmartGroup -> GroupRow(state, rule, node)
                    }
                }
            }
        }
        FlowRow(
            Modifier.padding(top = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedButton(onClick = { rule.addCondition(group) }) {
                Icon(Icons.Add, contentDescription = null, modifier = Modifier.size(18.dp).padding(end = 4.dp))
                Text(stringResource(R.string.smart_add_condition_button))
            }
            OutlinedButton(onClick = { state.stack = state.stack.push(rule.addGroup(group).id) }) {
                Icon(Icons.Add, contentDescription = null, modifier = Modifier.size(18.dp).padding(end = 4.dp))
                Text(stringResource(R.string.smart_add_group_button))
            }
        }
        if (root) OrderSection(state, rule, words)
        PreviewSection(state, rule, group)
    }
}

@Composable
private fun EditorHeading(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.titleMedium,
        modifier = Modifier.padding(top = 24.dp, bottom = 8.dp).semantics { heading() },
    )
}

/** The list's name, or a group's, with its sentence as placeholder: only a label, it changes nothing that matches. */
@Composable
private fun NameField(state: SmartEditorState, rule: SmartRule, group: SmartGroup) {
    val root = group.parent == null
    OutlinedTextField(
        value = if (root) state.name else group.name,
        onValueChange = {
            val name = it.take(MAX_NAME)
            if (root) state.name = name else group.name = name
        },
        label = { Text(stringResource(if (root) R.string.name else R.string.smart_group_name)) },
        placeholder = if (root) null else state.sentences[group.id]?.let { { Text(it, maxLines = 2, overflow = TextOverflow.Ellipsis) } },
        enabled = !state.readOnly,
        singleLine = true,
        keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Sentences),
        modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
    )
}

/** The rule as its sentence, and that it stays as it is. */
@Composable
private fun ReadOnlyRule(state: SmartEditorState, rule: SmartRule) {
    val old = state.oldRule.orEmpty()
    val sentence by produceState<String?>(null, old) { value = state.editor.describeRule(rule.isAlbums, old) }
    Text(
        stringResource(R.string.smart_read_only),
        style = MaterialTheme.typography.bodyMedium,
        modifier = Modifier.padding(top = 16.dp),
    )
    Card(Modifier.fillMaxWidth().padding(top = 12.dp)) {
        Text(sentence ?: old, style = MaterialTheme.typography.bodyLarge, modifier = Modifier.padding(16.dp))
    }
}

/** All, any or none of the following. */
@Composable
private fun ModePicker(group: SmartGroup) {
    val labels = listOf(
        stringResource(R.string.smart_mode_all),
        stringResource(R.string.smart_mode_any),
        stringResource(R.string.smart_mode_none),
    )
    Picker(
        labels,
        group.mode.ordinal,
        stringResource(R.string.smart_mode_name),
        onSelect = { group.mode = SmartMode.entries[it] },
        modifier = Modifier.padding(top = 12.dp),
    )
}

/**
 * A button showing [options]' [selected] one, which opens the list of them; [name] is what it
 * is, for accessibility. [selected] may be -1, for nothing yet.
 */
@Composable
private fun Picker(
    options: List<String>,
    selected: Int,
    name: String,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
) {
    var open by remember { mutableStateOf(false) }
    Box(modifier) {
        OutlinedButton(
            onClick = { open = true },
            enabled = enabled,
            modifier = Modifier.semantics { contentDescription = "$name, ${options.getOrNull(selected).orEmpty()}" },
        ) {
            Text(options.getOrNull(selected).orEmpty(), maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.widthIn(max = 240.dp))
            Icon(Icons.ExpandMore, contentDescription = null, modifier = Modifier.padding(start = 4.dp).size(18.dp))
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            for ((i, option) in options.withIndex()) {
                DropdownMenuItem(
                    text = {
                        Text(option, color = if (i == selected) MaterialTheme.colorScheme.primary else Color.Unspecified)
                    },
                    onClick = {
                        open = false
                        if (i != selected) onSelect(i)
                    },
                )
            }
        }
    }
}

/** What a row matches on its own, if it's been counted as it now stands; empty otherwise. */
@Composable
private fun RowCount(state: SmartEditorState, rule: SmartRule, node: SmartNode) {
    val now = if (node is SmartGroup && node.kids.isEmpty()) null else rule.matchJson(node, lenient = false)
    val counted = state.counts[node.id]?.takeIf { now != null && it.first == now }?.second
    val text = counted?.toString().orEmpty()
    val description = counted?.let {
        pluralStringResource(if (rule.isAlbums) R.plurals.smart_albums_match else R.plurals.smart_tracks_match, it, it)
    }
    Text(
        text,
        style = MaterialTheme.typography.labelLarge,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.widthIn(min = 32.dp).semantics { if (description != null) contentDescription = description },
    )
}

/**
 *  [Field v] [op v]           412  x
 *  [value                          ]
 *  Needs a value                       <- its problem, or a hint
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ConditionRow(state: SmartEditorState, rule: SmartRule, words: SmartWords, c: SmartCondition) {
    val problem = rule.problem(c)
    val bring = remember { BringIntoViewRequester() }
    val focus = remember { FocusRequester() }
    LaunchedEffect(state.focus) {
        if (state.focus == c.id) {
            delay(300) // once its page has slid in
            bring.bringIntoView()
            try {
                focus.requestFocus()
            } catch (e: IllegalStateException) {
                // a value with no box to type in (a picker): brought into view is enough
            }
            state.focus = null
        }
    }
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
        modifier = Modifier.fillMaxWidth().bringIntoViewRequester(bring),
    ) {
        Column(Modifier.padding(start = 12.dp, top = 8.dp, bottom = 8.dp, end = 4.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FlowRow(
                    Modifier.weight(1f),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    Picker(words.fields, c.field, stringResource(R.string.smart_field_name), onSelect = { rule.setField(c, it) })
                    Picker(words.ops[c.field], c.op, stringResource(R.string.smart_op_name), onSelect = { c.op = it })
                }
                RowCount(state, rule, c)
                IconButton(onClick = { rule.remove(c) }) {
                    Icon(Icons.Clear, contentDescription = stringResource(R.string.smart_remove_condition))
                }
            }
            ConditionValues(rule, c, focus)
            ConditionHint(rule, c, problem)
        }
    }
}

/** The value's boxes or pickers for [c]'s field and operator; [focus] goes to the first box. */
@Composable
private fun ConditionValues(rule: SmartRule, c: SmartCondition, focus: FocusRequester) {
    val type = rule.type(c)
    val valueName = stringResource(R.string.smart_value_name)
    val modifier = Modifier.padding(top = 8.dp, end = 8.dp)
    when {
        rule.takesNoValue(c) -> Unit
        rule.isChoice(c) -> {
            val options = when (type) {
                "bool" -> listOf(stringResource(R.string.smart_value_yes), stringResource(R.string.smart_value_no))
                "playlist" -> rule.playlists.map { it.name }
                "group" -> rule.groups.map { it.name }
                else -> SmartRule.FORMATS.map { it.second }
            }
            val extra = c.extra
            val all = when {
                extra == null -> options
                type == "format" -> options + extra.uppercase()
                else -> options + stringResource(R.string.smart_deleted_member)
            }
            val selected = when {
                type == "bool" -> if (c.choice != 0) 0 else 1
                c.choice == SmartRule.CHOICE_EXTRA -> options.size
                else -> c.choice
            }
            Picker(
                all,
                selected,
                valueName,
                onSelect = { i ->
                    c.choice = when {
                        type == "bool" -> if (i == 0) 1 else 0
                        i < options.size -> i
                        else -> SmartRule.CHOICE_EXTRA
                    }
                },
                enabled = all.isNotEmpty(),
                modifier = modifier.focusRequester(focus),
            )
        }
        (type == "int" || type == "duration") && rule.opOf(c) == "between" -> Row(modifier, verticalAlignment = Alignment.CenterVertically) {
            ValueBox(c.value, { c.value = it }, type, valueName, Modifier.weight(1f).focusRequester(focus))
            Text(stringResource(R.string.smart_between_and), modifier = Modifier.padding(horizontal = 8.dp))
            ValueBox(c.value2, { c.value2 = it }, type, stringResource(R.string.smart_value_high_name), Modifier.weight(1f))
        }
        rule.isWithin(c) -> Row(modifier, verticalAlignment = Alignment.CenterVertically) {
            ValueBox(c.value, { c.value = it }, "int", valueName, Modifier.width(120.dp).focusRequester(focus))
            Picker(
                listOf(
                    stringResource(R.string.smart_unit_days),
                    stringResource(R.string.smart_unit_weeks),
                    stringResource(R.string.smart_unit_months),
                ),
                c.unit.ordinal,
                stringResource(R.string.smart_unit_name),
                onSelect = { c.unit = SmartUnit.entries[it] },
                modifier = Modifier.padding(start = 8.dp),
            )
        }
        type == "date" -> DateButton(c, modifier.focusRequester(focus))
        else -> ValueBox(c.value, { c.value = it }, type, valueName, modifier.fillMaxWidth().focusRequester(focus))
    }
}

@Composable
private fun ValueBox(value: String, onChange: (String) -> Unit, type: String, name: String, modifier: Modifier) {
    OutlinedTextField(
        value = value,
        onValueChange = { onChange(it.take(256)) },
        singleLine = true,
        placeholder = if (type == "duration") ({ Text(stringResource(R.string.smart_duration_cue)) }) else null,
        keyboardOptions = KeyboardOptions(
            keyboardType = when (type) {
                "int" -> KeyboardType.Number
                "duration" -> KeyboardType.Ascii // for the colon
                else -> KeyboardType.Text
            },
        ),
        modifier = modifier.semantics { contentDescription = name },
    )
}

/** A before or after condition's date, which opens a date picker. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DateButton(c: SmartCondition, modifier: Modifier) {
    var open by remember { mutableStateOf(false) }
    val text = remember(c.date) { DateFormat.getDateInstance(DateFormat.MEDIUM).format(Date(c.date)) }
    OutlinedButton(onClick = { open = true }, modifier = modifier) {
        Icon(Icons.Event, contentDescription = null, modifier = Modifier.padding(end = 8.dp).size(18.dp))
        Text(text)
    }
    if (open) {
        // The picker works in UTC days; the rule in local midnights.
        val picker = rememberDatePickerState(initialSelectedDateMillis = utcDayOf(c.date))
        DatePickerDialog(
            onDismissRequest = { open = false },
            confirmButton = {
                TextButton(onClick = {
                    picker.selectedDateMillis?.let { c.date = localDayOf(it) }
                    open = false
                }) { Text(stringResource(R.string.ok)) }
            },
            dismissButton = { TextButton(onClick = { open = false }) { Text(stringResource(R.string.cancel)) } },
        ) {
            DatePicker(picker, title = { Text(stringResource(R.string.smart_pick_date), Modifier.padding(start = 24.dp, top = 16.dp)) })
        }
    }
}

// Local midnight of a day as the UTC midnight of the same date, and back.
private fun utcDayOf(local: Long): Long {
    val l = Calendar.getInstance().apply { timeInMillis = local }
    return Calendar.getInstance(TimeZone.getTimeZone("UTC")).apply {
        clear()
        set(l.get(Calendar.YEAR), l.get(Calendar.MONTH), l.get(Calendar.DAY_OF_MONTH))
    }.timeInMillis
}

private fun localDayOf(utc: Long): Long {
    val u = Calendar.getInstance(TimeZone.getTimeZone("UTC")).apply { timeInMillis = utc }
    return Calendar.getInstance().apply {
        clear()
        set(u.get(Calendar.YEAR), u.get(Calendar.MONTH), u.get(Calendar.DAY_OF_MONTH))
    }.timeInMillis
}

/** A condition's line beneath: its problem, else that it depends on this device, else that a negative text condition takes in what has no value. */
@Composable
private fun ConditionHint(rule: SmartRule, c: SmartCondition, problem: SmartProblem?) {
    val field = rule.fields[c.field].name
    val inText = IN_TEXT[field]
    val hint = when {
        problem != null -> problemText(problem)
        field == "on_device" || field == "format" -> stringResource(R.string.smart_hint_device)
        inText != null && (rule.opOf(c) == "is_not" || rule.opOf(c) == "not_contains") ->
            stringResource(if (rule.isAlbums) R.string.smart_hint_missing_albums else R.string.smart_hint_missing_tracks, stringResource(inText))
        else -> null
    } ?: return
    Text(
        hint,
        style = MaterialTheme.typography.bodySmall,
        color = if (problem != null) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(top = 4.dp, end = 8.dp),
    )
}

// The text fields' names inside a sentence, for a negative condition's hint.
private val IN_TEXT = mapOf(
    "title" to R.string.smart_field_title_in_text,
    "artist" to R.string.smart_field_artist_in_text,
    "album_artist" to R.string.smart_field_album_artist_in_text,
    "album" to R.string.smart_field_album_in_text,
    "genre" to R.string.smart_field_genre_in_text,
)

@Composable
private fun problemText(problem: SmartProblem): String = stringResource(
    when (problem) {
        SmartProblem.Value -> R.string.smart_problem_value
        SmartProblem.Number -> R.string.smart_problem_number
        SmartProblem.Duration -> R.string.smart_problem_duration
        SmartProblem.Choice -> R.string.smart_problem_choice
    },
)

/**
 *  Fresh stuff                         40  !  x  >
 *  Tracks where date added is in the last 30 days
 */
@Composable
private fun GroupRow(state: SmartEditorState, rule: SmartRule, g: SmartGroup) {
    val sentence = state.sentences[g.id]
    val sub = when {
        g.kids.isEmpty() -> stringResource(R.string.smart_group_empty_hint)
        g.isNamed -> sentence
        else -> null
    }
    val open = stringResource(R.string.smart_open_group)
    Card(
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
        modifier = Modifier.fillMaxWidth(),
    ) {
        ListItem(
            headlineContent = { Text(groupTitle(state, g), maxLines = 1, overflow = TextOverflow.Ellipsis) },
            supportingContent = sub?.let { { Text(it, maxLines = 2, overflow = TextOverflow.Ellipsis) } },
            trailingContent = {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    RowCount(state, rule, g)
                    if (rule.hasProblem(g)) {
                        Icon(
                            Icons.Warning,
                            contentDescription = stringResource(R.string.smart_group_has_problem),
                            tint = MaterialTheme.colorScheme.error,
                            modifier = Modifier.padding(horizontal = 4.dp),
                        )
                    }
                    IconButton(onClick = { rule.remove(g) }) {
                        Icon(Icons.Clear, contentDescription = stringResource(R.string.smart_remove_group))
                    }
                    Icon(Icons.ChevronRight, contentDescription = null)
                }
            },
            colors = ListItemDefaults.colors(containerColor = Color.Transparent),
            modifier = Modifier.clickable(onClickLabel = open) { state.stack = state.stack.push(g.id) },
        )
    }
}

/**
 *  Order
 *  Sort by        [least played first v]  x
 *  [+ Add sort order]
 *  Shuffled                          [   ]
 *  [x] Only the first      [10] [tracks]
 */
@Composable
private fun OrderSection(state: SmartEditorState, rule: SmartRule, words: SmartWords) {
    EditorHeading(stringResource(R.string.smart_order_heading))
    val keys = words.orders.map { it.first }
    val labels = words.orders.map { it.second }
    for ((i, k) in rule.sorts.withIndex()) {
        Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(bottom = 4.dp)) {
            Text(
                stringResource(if (i == 0) R.string.smart_sort_by else R.string.smart_sort_then),
                modifier = Modifier.widthIn(min = 64.dp).padding(end = 8.dp),
            )
            Box(Modifier.weight(1f)) {
                Picker(labels, keys.indexOf(k), stringResource(R.string.smart_sort_name), onSelect = { rule.sorts[i] = keys[it] })
            }
            IconButton(onClick = { rule.sorts.removeAt(i) }) {
                Icon(Icons.Clear, contentDescription = stringResource(R.string.smart_remove_sort))
            }
        }
    }
    OutlinedButton(
        onClick = {
            // The next sortable field not sorted on yet.
            val next = rule.sortable.indices.firstOrNull { s -> rule.sorts.none { it / 2 == s } } ?: 0
            rule.sorts += next * 2
        },
        enabled = rule.sorts.size < SmartRule.MAX_SORTS && rule.sorts.size < rule.sortable.size,
    ) {
        Icon(Icons.Add, contentDescription = null, modifier = Modifier.size(18.dp).padding(end = 4.dp))
        Text(stringResource(R.string.smart_add_sort_button))
    }
    Row(
        Modifier.fillMaxWidth().padding(top = 8.dp).clickable { rule.random = !rule.random }.padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(stringResource(R.string.smart_shuffled), modifier = Modifier.weight(1f))
        Switch(checked = rule.random, onCheckedChange = { rule.random = it })
    }
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Row(
            Modifier.weight(1f).clickable { rule.limited = !rule.limited },
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Checkbox(checked = rule.limited, onCheckedChange = { rule.limited = it })
            Text(stringResource(R.string.smart_limit))
        }
        val limitName = stringResource(R.string.smart_limit_count_name)
        OutlinedTextField(
            value = state.limitText,
            onValueChange = { text ->
                val digits = text.filter { it in '0'..'9' }.take(6)
                state.limitText = digits
                state.limitValue()?.let { rule.limit = it }
            },
            enabled = rule.limited,
            isError = rule.limited && state.limitValue() == null,
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            modifier = Modifier.width(80.dp).semantics { contentDescription = limitName },
        )
        Picker(
            listOf(
                stringResource(if (rule.isAlbums) R.string.smart_limit_albums else R.string.smart_limit_tracks),
                stringResource(R.string.smart_limit_minutes),
                stringResource(R.string.smart_limit_hours),
            ),
            rule.limitUnit,
            stringResource(R.string.smart_limit_unit_name),
            onSelect = { rule.limitUnit = it },
            modifier = Modifier.padding(start = 8.dp),
            enabled = rule.limited,
        )
    }
}

/** What the list holds, or what the group matches on its own: up to [PREVIEW_ROWS] of them, then "and N more". */
@Composable
private fun PreviewSection(state: SmartEditorState, rule: SmartRule, group: SmartGroup) {
    val root = group.parent == null
    if (!root && group.kids.isEmpty()) return
    EditorHeading(stringResource(if (root) R.string.smart_preview else R.string.smart_group_preview))
    val (page, preview) = state.preview ?: return
    if (page != group.id || preview == null) return
    val n = if (root) state.keptByLimit(rule, preview.count) else preview.count
    if (!root) {
        Text(
            pluralStringResource(if (rule.isAlbums) R.plurals.smart_group_albums else R.plurals.smart_group_tracks, n, n),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(bottom = 8.dp),
        )
    }
    val shown = preview.items.take(minOf(n, PREVIEW_ROWS))
    if (shown.isNotEmpty()) {
        Card(colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer)) {
            for ((i, item) in shown.withIndex()) {
                if (i > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                ListItem(
                    headlineContent = {
                        Text(item.title.ifEmpty { stringResource(R.string.untitled) }, maxLines = 1, overflow = TextOverflow.Ellipsis)
                    },
                    supportingContent = { Text(item.artist, maxLines = 1, overflow = TextOverflow.Ellipsis) },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                )
            }
        }
    }
    if (n > shown.size) {
        Text(
            pluralStringResource(R.plurals.smart_more, n - shown.size, n - shown.size),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.padding(top = 8.dp),
        )
    }
}

/**
 * The bottom: the conditions whose values are missing or wrong, each with Show, and how many
 * the rule matches ("40 tracks match · 10 in the list"), or Counting… while it's worked out.
 */
@Composable
private fun SmartBottom(state: SmartEditorState, rule: SmartRule) {
    val problems = rule.problems()
    val status = when {
        problems.isNotEmpty() -> null
        !state.counted -> stringResource(R.string.smart_counting)
        state.total >= 0 -> {
            val match = pluralStringResource(
                if (rule.isAlbums) R.plurals.smart_albums_match else R.plurals.smart_tracks_match,
                state.total,
                state.total,
            )
            if (rule.limited) {
                match + stringResource(R.string.separator_dot) +
                    stringResource(R.string.smart_in_list, state.keptByLimit(rule, state.total))
            } else {
                match
            }
        }
        else -> null
    }
    if (problems.isEmpty() && status == null) return
    Surface(color = MaterialTheme.colorScheme.surfaceContainer, tonalElevation = 2.dp, modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.navigationBarsPadding().padding(horizontal = 16.dp, vertical = 8.dp)) {
            if (problems.isNotEmpty()) {
                Card(
                    colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.errorContainer),
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Column(Modifier.padding(start = 16.dp, end = 4.dp, top = 8.dp, bottom = 8.dp)) {
                        Text(
                            pluralStringResource(R.plurals.smart_problems_title, problems.size, problems.size),
                            style = MaterialTheme.typography.titleSmall,
                            color = MaterialTheme.colorScheme.onErrorContainer,
                            modifier = Modifier.semantics { heading() },
                        )
                        for (c in problems.take(PROBLEM_ROWS)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Text(
                                    problemLine(state, rule, c),
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.onErrorContainer,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                    modifier = Modifier.weight(1f),
                                )
                                TextButton(onClick = { showProblem(state, c) }) { Text(stringResource(R.string.smart_problem_show)) }
                            }
                        }
                        if (problems.size > PROBLEM_ROWS) {
                            val more = problems.size - PROBLEM_ROWS
                            Text(
                                pluralStringResource(R.plurals.smart_more, more, more),
                                style = MaterialTheme.typography.bodyMedium,
                                color = MaterialTheme.colorScheme.onErrorContainer,
                            )
                        }
                    }
                }
            }
            if (status != null) {
                Text(
                    status,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = if (problems.isNotEmpty()) 8.dp else 0.dp),
                )
            }
        }
    }
}

/** "Fresh stuff › Artist: needs a value". */
@Composable
private fun problemLine(state: SmartEditorState, rule: SmartRule, c: SmartCondition): String {
    val groups = generateSequence(c.parent) { it.parent }.filter { it.parent != null }.toList().reversed()
    val separator = stringResource(R.string.smart_path_separator)
    val where = (groups.map { groupTitle(state, it) } + (CoreSmart.fieldLabel(rule.fields[c.field].name) ?: rule.fields[c.field].name))
        .joinToString(separator)
    val problem = rule.problem(c) ?: return where
    return stringResource(R.string.smart_problem, where, problemText(problem))
}

/** Show on a problem: its group's page, over those above it, with the condition brought into view. */
private fun showProblem(state: SmartEditorState, c: SmartCondition) {
    val groups = generateSequence(c.parent) { it.parent }.filter { it.parent != null }.map { it.id }.toList().reversed()
    state.stack = BackStack.of(groups)
    state.focus = c.id
}

private data class Preset(val name: Int, val rule: String)

private val TrackPresets = listOf(
    Preset(
        R.string.smart_preset_recent,
        """{"v":1,"match":{"all":[{"field":"added","op":"within","value":{"days":30}}]},"order":[{"field":"added","dir":"desc"}]}""",
    ),
    Preset(
        R.string.smart_preset_least_played,
        """{"v":1,"match":{"all":[{"field":"on_device","op":"is","value":true}]},""" +
            """"order":[{"field":"plays","dir":"asc"},{"field":"last_played","dir":"asc"}],"limit":100}""",
    ),
    Preset(
        R.string.smart_preset_not_played,
        """{"v":1,"match":{"all":[{"field":"last_played","op":"not_within","value":{"days":180}}]},"order":[{"field":"last_played","dir":"asc"}]}""",
    ),
    Preset(
        R.string.smart_preset_flac,
        """{"v":1,"match":{"all":[{"field":"on_device","op":"is","value":true},{"field":"format","op":"is","value":"flac"}]}}""",
    ),
    Preset(R.string.smart_preset_blank, """{"v":1}"""),
)

private val AlbumPresets = listOf(
    Preset(
        R.string.smart_preset_recent_albums,
        """{"v":1,"match":{"all":[{"field":"added","op":"within","value":{"days":30}}]},"order":[{"field":"added","dir":"desc"}]}""",
    ),
    Preset(
        R.string.smart_preset_least_played_albums,
        """{"v":1,"match":{"all":[{"field":"on_device","op":"is","value":true}]},""" +
            """"order":[{"field":"plays","dir":"asc"},{"field":"last_played","dir":"asc"}],"limit":20}""",
    ),
    Preset(
        R.string.smart_preset_not_played_albums,
        """{"v":1,"match":{"all":[{"field":"last_played","op":"not_within","value":{"days":180}}]},"order":[{"field":"last_played","dir":"asc"}]}""",
    ),
    Preset(
        R.string.smart_preset_flac_albums,
        """{"v":1,"match":{"all":[{"field":"on_device","op":"is","value":true},{"field":"format","op":"is","value":"flac"}]}}""",
    ),
    Preset(R.string.smart_preset_blank, """{"v":1}"""),
)

/**
 *  Start from
 *  Recently added                             >
 *    Tracks where date added is in the last ...
 *  ...
 *  Blank                                      >
 */
@Composable
private fun PresetsPage(state: SmartEditorState, onBack: () -> Unit) {
    val presets = if (state.isAlbums) AlbumPresets else TrackPresets
    val sentences by produceState<Map<Int, String>>(emptyMap(), presets) {
        value = presets.filter { it.name != R.string.smart_preset_blank }
            .mapNotNull { p -> state.editor.describeRule(state.isAlbums, p.rule)?.let { p.name to it } }
            .toMap()
    }
    SmartPageFrame(state, null, null, onBack = onBack, onSave = {}) {
        Card(
            colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceContainer),
            modifier = Modifier.fillMaxWidth().padding(top = 8.dp),
        ) {
            for ((i, preset) in presets.withIndex()) {
                if (i > 0) HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                val name = stringResource(preset.name)
                ListItem(
                    headlineContent = { Text(name) },
                    supportingContent = sentences[preset.name]?.let { { Text(it) } },
                    trailingContent = { Icon(Icons.ChevronRight, contentDescription = null) },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                    modifier = Modifier.clickable {
                        state.start(if (preset.name == R.string.smart_preset_blank) "" else name, preset.rule)
                    },
                )
            }
        }
    }
}
