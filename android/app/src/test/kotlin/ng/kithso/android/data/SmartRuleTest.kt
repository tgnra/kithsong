package ng.kithso.android.data

import ng.kithso.core.SmartField
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The rule editor's model on its own, off the device: rules read in come back out as they went
 * in, and what the editor can't show, or what has a problem, is refused. The fields are the
 * core's for smart playlists (core/src/smart/smartlist.c), written out here as the JNI gives them.
 */
class SmartRuleTest {
    private val text = listOf("is", "is_not", "contains", "not_contains", "starts_with", "empty", "not_empty")
    private val int = listOf("=", "!=", "<", "<=", ">", ">=", "between", "empty", "not_empty")
    private val date = listOf("before", "after", "within", "not_within", "empty", "not_empty")
    private val fields = listOf(
        SmartField("title", "text", text, true),
        SmartField("artist", "text", text, true),
        SmartField("genre", "text", text, false),
        SmartField("year", "int", int, true),
        SmartField("duration", "duration", int, true),
        SmartField("added", "date", date, true),
        SmartField("plays", "int", int, true),
        SmartField("last_played", "date", date, true),
        SmartField("on_device", "bool", listOf("is"), false),
        SmartField("format", "format", listOf("is", "is_not"), false),
        SmartField("playlist", "playlist", listOf("is", "is_not"), false),
    )
    private val playlists = listOf(PlaylistChoice("p1", "Road trip"))

    private fun rule() = SmartRule(false, fields, { true }, playlists, emptyList(), today = { 1_700_000_000_000 })

    private fun roundTrip(json: String) {
        val r = rule()
        assertTrue("loads $json", r.load(json))
        assertEquals(json, r.ruleJson())
    }

    @Test
    fun readsAndWritesRulesUnchanged() {
        roundTrip("""{"v":1}""")
        roundTrip("""{"v":1,"match":{"all":[{"field":"genre","op":"contains","value":"jazz"}]},"order":[{"field":"plays","dir":"asc"}],"limit":10}""")
        roundTrip("""{"v":1,"limit_duration":3600}""")
        roundTrip("""{"v":1,"order":[{"field":"plays","dir":"asc"}],"limit_duration":5400}""")
        roundTrip("""{"v":1,"match":{"any":[{"field":"year","op":"between","value":[1990,1999]},{"field":"duration","op":">","value":330}]}}""")
        roundTrip("""{"v":1,"match":{"all":[{"field":"last_played","op":"not_within","value":{"days":180}},{"field":"added","op":"after","value":1700000000000}]},"order":[{"field":"random","seed":42}]}""")
        roundTrip("""{"v":1,"match":{"all":[{"field":"on_device","op":"is","value":true},{"field":"format","op":"is","value":"flac"},{"field":"playlist","op":"is_not","value":"p1"}]}}""")
        // Named groups, a none-of group, and an empty one, as docs/schema.md has them.
        roundTrip(
            """{"v":1,"match":{"all":[{"name":"Fresh stuff","any":[{"field":"added","op":"within","value":{"days":14}}]},""" +
                """{"name":"Old","not":{"any":[{"field":"plays","op":">","value":3}]}},{"all":[]}]}}""",
        )
    }

    @Test
    fun keepsWhatItDoesNotOffer() {
        // A deleted playlist's uid and another format stay as they are.
        roundTrip("""{"v":1,"match":{"all":[{"field":"playlist","op":"is","value":"gone"},{"field":"format","op":"is","value":"ogg"}]}}""")
    }

    @Test
    fun showsANotAsTheNegativeOperator() {
        val r = rule()
        assertTrue(r.load("""{"v":1,"match":{"all":[{"not":{"field":"genre","op":"contains","value":"rock"}},{"not":{"field":"on_device","op":"is","value":true}}]}}"""))
        assertEquals(
            """{"v":1,"match":{"all":[{"field":"genre","op":"not_contains","value":"rock"},{"field":"on_device","op":"is","value":false}]}}""",
            r.ruleJson(),
        )
    }

    @Test
    fun refusesWhatItCannotShow() {
        assertFalse(rule().load("""{"v":2}"""))
        assertFalse(rule().load("""{"v":1,"match":{"not":{"all":[]}}}"""))
        assertFalse(rule().load("""{"v":1,"match":{"all":[{"not":{"field":"title","op":"starts_with","value":"a"}}]}}"""))
        assertFalse(rule().load("not json"))
    }

    @Test
    fun weeksReadAsWeeks() {
        val r = rule()
        assertTrue(r.load("""{"v":1,"match":{"all":[{"field":"added","op":"within","value":{"days":28}}]}}"""))
        val c = r.root.kids.single() as SmartCondition
        assertEquals("4", c.value)
        assertEquals(SmartUnit.Weeks, c.unit)
    }

    @Test
    fun problemsKeepItFromBeingWritten() {
        val r = rule()
        val c = r.addCondition(r.root)
        assertEquals("genre", r.fields[c.field].name)
        assertEquals(SmartProblem.Value, r.problem(c))
        assertNull(r.ruleJson())
        assertEquals(listOf(c), r.problems())
        c.value = " jazz "
        assertNull(r.problem(c))
        assertEquals("""{"v":1,"match":{"all":[{"field":"genre","op":"contains","value":"jazz"}]}}""", r.ruleJson())

        r.setField(c, 4) // duration: a new type, so new values
        assertEquals(">", r.opOf(c))
        c.value = "3:75"
        assertEquals(SmartProblem.Duration, r.problem(c))
        c.value = "3:30"
        assertEquals("""{"v":1,"match":{"all":[{"field":"duration","op":">","value":210}]}}""", r.ruleJson())
    }

    @Test
    fun aGroupWithAProblemCanStillBeDescribed() {
        val r = rule()
        val g = r.addGroup(r.root)
        assertEquals(SmartMode.Any, g.mode)
        val good = r.addCondition(g)
        good.value = "jazz"
        r.addCondition(g) // blank
        assertTrue(r.hasProblem(g))
        assertTrue(r.saysSomething(g))
        assertNull(r.matchJson(g, lenient = false))
        assertEquals("""{"v":1,"match":{"any":[{"field":"genre","op":"contains","value":"jazz"}]}}""", r.matchJson(g, lenient = true))
    }

    @Test
    fun durations() {
        assertEquals(180L, SmartRule.parseDuration("3"))
        assertEquals(210L, SmartRule.parseDuration(" 3:30 "))
        assertEquals(3723L, SmartRule.parseDuration("1:02:03"))
        assertNull(SmartRule.parseDuration("1:"))
        assertNull(SmartRule.parseDuration("1:60"))
        assertEquals("3:30", SmartRule.formatDuration(210))
        assertEquals("1:02:03", SmartRule.formatDuration(3723))
    }

    @Test
    fun json() {
        val value = SmartJson.parse("""{"a":[1,2.5,"x\"é\n",true,null],"b":{}}""")
        assertEquals(mapOf("a" to listOf(1L, 2.5, "x\"é\n", true, null), "b" to emptyMap<String, Any?>()), value)
        assertEquals("""{"a":[1,2.5,"x\"é\n",true,null],"b":{}}""", SmartJson.write(value))
    }
}
