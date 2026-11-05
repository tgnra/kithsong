package ng.kithso.android.data

import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import ng.kithso.core.SmartField
import java.util.Calendar
import java.util.Locale
import kotlin.random.Random

/** How a group's conditions combine: all, any or none of them must hold. */
enum class SmartMode { All, Any, None }

/** What a "in the last" number counts: days, weeks or months (of 30 days). */
enum class SmartUnit(val days: Int) { Days(1), Weeks(7), Months(30) }

/** What's wrong with a condition's value, which keeps the rule from being saved. */
enum class SmartProblem { Value, Number, Duration, Choice }

/** A condition or a group of them in a [SmartRule]; [id] tells them apart. [parent] is null for the list's own group. */
@Stable
sealed class SmartNode(val id: Int, val parent: SmartGroup?)

/**
 * A group of conditions. [name] is only a label, as typed (written trimmed, and only for a
 * group inside the list's own); an empty group is left out of the rule by the core.
 */
@Stable
class SmartGroup internal constructor(id: Int, parent: SmartGroup?) : SmartNode(id, parent) {
    var mode by mutableStateOf(SmartMode.All)
    var name by mutableStateOf("")
    val kids = mutableStateListOf<SmartNode>()

    val isNamed: Boolean get() = name.isNotBlank()
}

/**
 * A condition: [field] and [op] index [SmartRule.fields] and that field's ops. Its value is kept
 * as the editor shows it: [value] (and [value2], the high end of between) as typed, a number
 * of [unit]s back for within, a [date] (local midnight, Unix ms) for before and after, and
 * [choice] for Yes (1) or No (0), or a playlist, album group or format by index, or
 * [SmartRule.CHOICE_EXTRA] for [extra] (one the rule names that isn't offered: a deleted
 * playlist's or group's uid, or another format).
 */
@Stable
class SmartCondition internal constructor(id: Int, parent: SmartGroup) : SmartNode(id, parent) {
    var field by mutableIntStateOf(0)
    var op by mutableIntStateOf(0)
    var value by mutableStateOf("")
    var value2 by mutableStateOf("")
    var unit by mutableStateOf(SmartUnit.Days)
    var date by mutableLongStateOf(0L)
    var choice by mutableIntStateOf(SmartRule.CHOICE_NONE)
    var extra by mutableStateOf<String?>(null)
}

/**
 * A smart playlist's rule, or if [isAlbums] a smart album group's, as the editor holds it,
 * ported from the Linux editor's (desktop/linux/src/smart_editor.c): the list's own group of
 * conditions ([root]) with groups nested in it, its sort orders ([sorts]), whether it's
 * [random] and its [limit]. [load] reads a rule in, and [ruleJson] writes it back out in the
 * format docs/schema.md gives, with group names and empty groups as written there.
 *
 * [fields] are the core's (CoreSmart.fields); [hasOrderLabel] says which of the sortable ones
 * have words to show them by, as only those are offered. [playlists] and [groups] are what a
 * playlist or album group condition offers. [today] is local midnight, for a new date.
 */
@Stable
class SmartRule(
    val isAlbums: Boolean,
    val fields: List<SmartField>,
    hasOrderLabel: (String) -> Boolean,
    val playlists: List<PlaylistChoice>,
    val groups: List<PlaylistChoice>,
    private val today: () -> Long = ::localMidnight,
) {
    private var nextId = 0

    /** The sortable fields, into [fields]. */
    val sortable: List<Int> = fields.indices.filter { fields[it].sortable && hasOrderLabel(fields[it].name) }

    val root = SmartGroup(++nextId, null)

    /** The sort orders in turn, each a [sortable] index * 2, plus 1 for descending. */
    val sorts = mutableStateListOf<Int>()
    var random by mutableStateOf(false)
    var seed: Long = Random.nextLong(0, Int.MAX_VALUE.toLong()) // should Shuffled be turned on
    var limited by mutableStateOf(false)
    var limit by mutableIntStateOf(if (isAlbums) 10 else 25)

    /** What [limit] counts: tracks or albums ([LIMIT_ITEMS]), [LIMIT_MINUTES] or [LIMIT_HOURS] of music. */
    var limitUnit by mutableIntStateOf(LIMIT_ITEMS)

    fun type(c: SmartCondition): String = fields[c.field].type
    fun opOf(c: SmartCondition): String = fields[c.field].ops[c.op]
    fun takesNoValue(c: SmartCondition): Boolean = opOf(c) == "empty" || opOf(c) == "not_empty"
    fun isChoice(c: SmartCondition): Boolean = type(c) in CHOICE_TYPES
    fun isWithin(c: SmartCondition): Boolean = type(c) == "date" && (opOf(c) == "within" || opOf(c) == "not_within")

    private fun findField(name: String?): Int = fields.indexOfFirst { it.name == name }
    private fun findOp(field: Int, op: String?): Int = fields[field].ops.indexOf(op)

    // A new condition's operator for its field's type: the one most rules want.
    private fun defaultOp(field: Int): Int {
        val op = when (fields[field].type) {
            "text" -> "contains"
            "int" -> "="
            "duration" -> ">"
            "date" -> "within"
            else -> "is"
        }
        return findOp(field, op).coerceAtLeast(0)
    }

    // The first choice for a condition's picker, or CHOICE_NONE if it has nothing to pick.
    private fun firstChoice(c: SmartCondition): Int = when (type(c)) {
        "bool" -> 1
        "playlist" -> if (playlists.isNotEmpty()) 0 else CHOICE_NONE
        "group" -> if (groups.isNotEmpty()) 0 else CHOICE_NONE
        else -> 0
    }

    // Values that suit c's field, after it's changed to another type.
    private fun resetValues(c: SmartCondition) {
        c.value = if (type(c) == "date") "30" else ""
        c.value2 = ""
        c.extra = null
        c.unit = SmartUnit.Days
        c.date = today()
        c.choice = firstChoice(c)
    }

    /** A new condition at the end of [parent]: Genre for tracks, which most rules start from, else Artist. */
    fun addCondition(parent: SmartGroup): SmartCondition {
        val c = SmartCondition(++nextId, parent)
        c.field = findField(if (isAlbums) "artist" else "genre").coerceAtLeast(0)
        c.op = defaultOp(c.field)
        resetValues(c)
        parent.kids += c
        return c
    }

    /** A new, empty group at the end of [parent], combining the other way round, else why a group. */
    fun addGroup(parent: SmartGroup): SmartGroup {
        val g = SmartGroup(++nextId, parent)
        g.mode = if (parent.mode == SmartMode.All) SmartMode.Any else SmartMode.All
        parent.kids += g
        return g
    }

    fun remove(node: SmartNode) {
        node.parent?.kids?.remove(node)
    }

    /** Changes [c]'s field, keeping its operator and value if the new one is of the same type. */
    fun setField(c: SmartCondition, field: Int) {
        if (field == c.field) return
        val wasType = type(c)
        val wasOp = opOf(c)
        val sameType = wasType == fields[field].type
        c.field = field
        val op = if (sameType) findOp(field, wasOp) else -1
        c.op = if (op >= 0) op else defaultOp(field)
        if (!sameType) {
            resetValues(c) // "jazz" means nothing as a length
        } else if (isChoice(c)) {
            c.choice = firstChoice(c)
            c.extra = null
        }
    }

    /** The node with [id] anywhere in the rule, or null. */
    fun find(id: Int, from: SmartGroup = root): SmartNode? {
        if (from.id == id) return from
        for (k in from.kids) {
            if (k.id == id) return k
            if (k is SmartGroup) find(id, k)?.let { return it }
        }
        return null
    }

    // --- Problems ---

    /** What's wrong with [c]'s value, or null. */
    fun problem(c: SmartCondition): SmartProblem? {
        if (takesNoValue(c)) return null
        return when (type(c)) {
            "text" -> if (c.value.isBlank()) SmartProblem.Value else null
            "int", "duration" -> {
                val dur = type(c) == "duration"
                val values = if (opOf(c) == "between") listOf(c.value, c.value2) else listOf(c.value)
                values.firstNotNullOfOrNull { v ->
                    when {
                        v.isBlank() -> SmartProblem.Value
                        (if (dur) parseDuration(v) else parseInt(v)) == null ->
                            if (dur) SmartProblem.Duration else SmartProblem.Number
                        else -> null
                    }
                }
            }
            "date" -> if (!isWithin(c)) {
                null
            } else if (c.value.isBlank()) {
                SmartProblem.Value
            } else {
                val n = parseInt(c.value)
                if (n != null && n * c.unit.days <= MAX_DAYS) null else SmartProblem.Number
            }
            "bool" -> null
            else -> if (c.choice == CHOICE_NONE) SmartProblem.Choice else null
        }
    }

    /** The conditions with a problem, in the rule's order. */
    fun problems(): List<SmartCondition> = ArrayList<SmartCondition>().also { collect(root, it) }

    private fun collect(g: SmartGroup, out: MutableList<SmartCondition>) {
        for (k in g.kids) {
            if (k is SmartGroup) collect(k, out) else if (problem(k as SmartCondition) != null) out += k
        }
    }

    /** Whether anything in [g] has a problem. */
    fun hasProblem(g: SmartGroup): Boolean =
        g.kids.any { if (it is SmartGroup) hasProblem(it) else problem(it as SmartCondition) != null }

    /** Whether [g] holds a condition without a problem: else its sentence would be "All tracks". */
    fun saysSomething(g: SmartGroup): Boolean =
        g.kids.any { if (it is SmartGroup) saysSomething(it) else problem(it as SmartCondition) == null }

    // --- The rule as JSON ---

    // c's condition, or null if it has a problem.
    private fun condJson(c: SmartCondition): Map<String, Any?>? {
        if (problem(c) != null) return null
        val value: Any? = when {
            takesNoValue(c) -> null
            type(c) == "text" -> c.value.trim()
            type(c) == "int" || type(c) == "duration" -> {
                val parse: (String) -> Long? = if (type(c) == "duration") ::parseDuration else ::parseInt
                if (opOf(c) == "between") listOf(parse(c.value), parse(c.value2)) else parse(c.value)
            }
            isWithin(c) -> linkedMapOf("days" to parseInt(c.value)!! * c.unit.days)
            type(c) == "date" -> c.date
            type(c) == "bool" -> c.choice != 0
            else -> {
                val i = c.choice
                when {
                    i == CHOICE_EXTRA -> c.extra
                    i < 0 -> null
                    type(c) == "playlist" -> playlists.getOrNull(i)?.uid
                    type(c) == "group" -> groups.getOrNull(i)?.uid
                    else -> FORMATS.getOrNull(i)?.first
                } ?: return null
            }
        }
        return linkedMapOf<String, Any?>("field" to fields[c.field].name, "op" to opOf(c)).apply {
            if (value != null) put("value", value)
        }
    }

    // {"name", "all": [...]}, {"name", "any": [...]} or, for none, {"name", "not": {"any": [...]}}.
    // Lenient leaves out what has a problem, and empty groups, as for a sentence; otherwise a
    // problem anywhere makes it null.
    private fun groupJson(g: SmartGroup, lenient: Boolean): Map<String, Any?>? {
        val list = ArrayList<Any?>()
        for (k in g.kids) {
            if (lenient && k is SmartGroup && k.kids.isEmpty()) continue
            val v = nodeJson(k, lenient)
            if (v != null) list += v else if (!lenient) return null
        }
        val obj = LinkedHashMap<String, Any?>()
        if (g.isNamed && g.parent != null) obj["name"] = g.name.trim()
        when (g.mode) {
            SmartMode.All -> obj["all"] = list
            SmartMode.Any -> obj["any"] = list
            SmartMode.None -> obj["not"] = linkedMapOf("any" to list)
        }
        return obj
    }

    private fun nodeJson(n: SmartNode, lenient: Boolean): Map<String, Any?>? =
        if (n is SmartGroup) groupJson(n, lenient) else condJson(n as SmartCondition)

    /**
     * {"v":1,"match":<node>}, for counting or describing one condition or group on its own; null
     * if not [lenient] and it has a problem.
     */
    fun matchJson(node: SmartNode, lenient: Boolean): String? {
        val match = nodeJson(node, lenient) ?: return null
        return SmartJson.write(linkedMapOf("v" to 1, "match" to match))
    }

    /** The whole rule as it stands, with its limit if [withLimit]; null if anything in it has a problem. */
    fun ruleJson(withLimit: Boolean = true): String? {
        val out = linkedMapOf<String, Any?>("v" to 1)
        if (root.kids.isNotEmpty()) out["match"] = groupJson(root, lenient = false) ?: return null
        val order = ArrayList<Any?>()
        for (k in sorts) {
            order += linkedMapOf("field" to fields[sortable[k / 2]].name, "dir" to if (k % 2 == 1) "desc" else "asc")
        }
        if (random) order += linkedMapOf("field" to "random", "seed" to seed)
        if (order.isNotEmpty()) out["order"] = order
        if (withLimit && limited && limit > 0) {
            if (limitUnit == LIMIT_ITEMS) out["limit"] = limit
            else out["limit_duration"] = limit.toLong() * (if (limitUnit == LIMIT_HOURS) 3600 else 60)
        }
        return SmartJson.write(out)
    }

    // --- Reading a rule in ---

    /**
     * Reads [rule], which the core has checked (CoreSmart.isValid), into [root] and the order.
     * False if the editor can't show it: a newer version, or a "not" with no negative operator
     * to show it as. Call it once, on a new SmartRule.
     */
    fun load(rule: String): Boolean {
        val doc = try {
            SmartJson.parse(rule) as? Map<*, *>
        } catch (e: IllegalArgumentException) {
            null
        } ?: return false
        if ((doc["v"] as? Number)?.toLong() != 1L) return false
        val match = doc["match"]
        if (match != null) {
            val group = groupOf(match)
            if (group != null) {
                root.mode = group.first
                if (!group.second.all { loadNode(root, it) }) return false
            } else if (!loadNode(root, match)) { // one condition on its own
                return false
            }
        }
        for (key in doc["order"] as? List<*> ?: emptyList<Any?>()) {
            val k = key as? Map<*, *> ?: return false
            val field = k["field"] as? String
            if (field == "random") {
                random = true
                seed = (k["seed"] as? Number)?.toLong() ?: 0
                continue
            }
            val f = findField(field)
            val s = sortable.indexOf(f)
            if (f < 0 || s < 0 || sorts.size >= MAX_SORTS) return false
            sorts += s * 2 + if (k["dir"] == "desc") 1 else 0
        }
        (doc["limit"] as? Number)?.let {
            limited = true
            limit = it.toLong().coerceIn(1, Int.MAX_VALUE.toLong()).toInt()
        }
        (doc["limit_duration"] as? Number)?.let {
            val seconds = it.toLong()
            limited = true
            limitUnit = if (seconds % 3600 == 0L) LIMIT_HOURS else LIMIT_MINUTES
            val n = if (limitUnit == LIMIT_HOURS) seconds / 3600 else (seconds + 59) / 60
            limit = n.coerceIn(1, Int.MAX_VALUE.toLong()).toInt()
        }
        return true
    }

    // {"all": [...]}, {"any": [...]} or {"not": {"any": [...]}}, each maybe with a "name": its
    // mode and list. Null for anything else.
    private fun groupOf(v: Any?): Pair<SmartMode, List<*>>? {
        val obj = v as? Map<*, *> ?: return null
        val not = obj["not"]
        if (not != null) return ((not as? Map<*, *>)?.get("any") as? List<*>)?.let { SmartMode.None to it }
        (obj["all"] as? List<*>)?.let { return SmartMode.All to it }
        (obj["any"] as? List<*>)?.let { return SmartMode.Any to it }
        return null
    }

    // v into a new node of parent's. False if the editor can't show it.
    private fun loadNode(parent: SmartGroup, v: Any?): Boolean {
        val group = groupOf(v)
        val obj = v as? Map<*, *> ?: return false
        if (group != null) {
            val g = SmartGroup(++nextId, parent)
            g.mode = group.first
            g.name = obj["name"] as? String ?: ""
            parent.kids += g
            return group.second.all { loadNode(g, it) }
        }
        val not = obj["not"] as? Map<*, *>
        if (not != null && ("all" in not || "any" in not || "not" in not)) return false
        return loadCondition(parent, not ?: obj, negated = not != null)
    }

    // A condition (or a "not" around one, if negated) into a new node of parent's; false if the
    // editor can't show it. The rule has been validated, so values have the right shapes.
    private fun loadCondition(parent: SmartGroup, leaf: Map<*, *>, negated: Boolean): Boolean {
        val field = findField(leaf["field"] as? String)
        if (field < 0) return false
        val type = fields[field].type
        var op = leaf["op"] as? String
        val value = leaf["value"]
        var yes = value == true
        if (negated && type == "bool") yes = !yes else if (negated) op = negatedOp(op)
        val opIndex = findOp(field, op)
        if (opIndex < 0) return false
        val c = SmartCondition(++nextId, parent)
        c.field = field
        c.op = opIndex
        resetValues(c)
        when {
            type == "text" && value is String -> c.value = value
            type == "int" || type == "duration" -> {
                val values = if (value is List<*>) value else listOf(value)
                for ((i, v) in values.take(2).withIndex()) {
                    val n = (v as? Number)?.toLong() ?: continue
                    if (n < 0) return false // the boxes take digits only
                    val text = if (type == "duration") formatDuration(n) else n.toString()
                    if (i == 0) c.value = text else c.value2 = text
                }
            }
            type == "date" && value is Map<*, *> -> {
                val days = (value["days"] as? Number)?.toLong() ?: 0
                val weeks = days >= 14 && days % 7 == 0L // as mc_smart_describe() says it
                c.value = (if (weeks) days / 7 else days).toString()
                c.unit = if (weeks) SmartUnit.Weeks else SmartUnit.Days
            }
            type == "date" && value is Number -> c.date = value.toLong()
            type == "bool" -> c.choice = if (yes) 1 else 0
            value is String -> {
                val found = when (type) {
                    "playlist" -> playlists.indexOfFirst { it.uid == value }
                    "group" -> groups.indexOfFirst { it.uid == value }
                    "format" -> FORMATS.indexOfFirst { it.first.equals(value, ignoreCase = true) }
                    else -> -1
                }
                if (found < 0) c.extra = value
                c.choice = if (found >= 0) found else CHOICE_EXTRA
            }
        }
        parent.kids += c
        return true
    }

    companion object {
        const val CHOICE_NONE = -1
        const val CHOICE_EXTRA = -2
        const val MAX_SORTS = 8
        const val MAX_DAYS = 1_000_000
        private val CHOICE_TYPES = setOf("bool", "playlist", "group", "format")

        /**
         * The formats a condition offers, as (value, label): the importer's file extensions
         * (tagger.c), which is what files.format holds. A rule naming another keeps it as an extra.
         */
        val FORMATS = listOf("flac" to "FLAC", "mp3" to "MP3", "opus" to "Opus", "wav" to "WAV", "aiff" to "AIFF", "aif" to "AIF")

        const val LIMIT_ITEMS = 0
        const val LIMIT_MINUTES = 1
        const val LIMIT_HOURS = 2

        // A "not" around a condition, as the negative operator that means the same: missing
        // values count as false either way, so only the ops the core writes as another's negation map.
        private val NEGATED = listOf("is" to "is_not", "contains" to "not_contains", "empty" to "not_empty", "=" to "!=", "within" to "not_within")

        private fun negatedOp(op: String?): String? =
            NEGATED.firstNotNullOfOrNull { (a, b) -> if (op == a) b else if (op == b) a else null }

        /** A whole number of at most 12 digits, ignoring spaces around it; null if it isn't one. */
        fun parseInt(text: String): Long? {
            val s = text.trim()
            if (s.isEmpty() || s.length > 12 || !s.all { it in '0'..'9' }) return null
            return s.toLong()
        }

        /** A length in seconds from m:ss or h:mm:ss, or a bare number of minutes; null if it isn't one. */
        fun parseDuration(text: String): Long? {
            val parts = text.trim().split(':')
            if (parts.size > 3 || parts.any { it.isEmpty() || it.length > 6 || !it.all { c -> c in '0'..'9' } }) return null
            val n = parts.map { it.toLong() }
            return when (n.size) {
                1 -> n[0] * 60
                2 -> if (n[1] < 60) n[0] * 60 + n[1] else null
                else -> if (n[1] < 60 && n[2] < 60) n[0] * 3600 + n[1] * 60 + n[2] else null
            }
        }

        // In ASCII digits whatever the language, as parseDuration reads them.
        fun formatDuration(s: Long): String =
            if (s >= 3600) String.format(Locale.ROOT, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60)
            else String.format(Locale.ROOT, "%d:%02d", s / 60, s % 60)

        /** Today at local midnight, as Unix ms. */
        fun localMidnight(): Long = Calendar.getInstance().apply {
            set(Calendar.HOUR_OF_DAY, 0)
            set(Calendar.MINUTE, 0)
            set(Calendar.SECOND, 0)
            set(Calendar.MILLISECOND, 0)
        }.timeInMillis
    }
}
