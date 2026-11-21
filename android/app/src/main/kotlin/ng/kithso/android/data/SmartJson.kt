package ng.kithso.android.data

/**
 * Just enough JSON for smart list rules (docs/schema.md, Smart lists): values are a Map with
 * its keys in order, a List, a String, a Long (a whole number), a Double, a Boolean or null.
 * The core checks every rule itself (mc_smart_validate), so this only has to read what it
 * takes and write what [SmartRule] makes. Plain Kotlin rather than org.json, so the rule
 * editor's model runs in unit tests off the device.
 */
internal object SmartJson {
    /** [text] as a value; IllegalArgumentException if it isn't JSON. */
    fun parse(text: String): Any? {
        val reader = Reader(text)
        val value = reader.value()
        reader.space()
        require(reader.at == text.length) { "JSON: trailing text at ${reader.at}" }
        return value
    }

    fun write(value: Any?): String = StringBuilder().also { write(it, value) }.toString()

    private fun write(out: StringBuilder, value: Any?) {
        when (value) {
            null -> out.append("null")
            is Boolean, is Int, is Long -> out.append(value.toString())
            is Double -> out.append(if (value % 1.0 == 0.0 && value in -1e15..1e15) value.toLong().toString() else value.toString())
            is String -> string(out, value)
            is Map<*, *> -> {
                out.append('{')
                var first = true
                for ((k, v) in value) {
                    if (!first) out.append(',')
                    first = false
                    string(out, k.toString())
                    out.append(':')
                    write(out, v)
                }
                out.append('}')
            }
            is List<*> -> {
                out.append('[')
                value.forEachIndexed { i, v ->
                    if (i > 0) out.append(',')
                    write(out, v)
                }
                out.append(']')
            }
            else -> throw IllegalArgumentException("JSON: can't write ${value::class}")
        }
    }

    private fun string(out: StringBuilder, s: String) {
        out.append('"')
        for (c in s) {
            when (c) {
                '"' -> out.append("\\\"")
                '\\' -> out.append("\\\\")
                '\n' -> out.append("\\n")
                '\r' -> out.append("\\r")
                '\t' -> out.append("\\t")
                else -> if (c < ' ') out.append("\\u").append(c.code.toString(16).padStart(4, '0')) else out.append(c)
            }
        }
        out.append('"')
    }

    private class Reader(val text: String) {
        var at = 0

        fun space() {
            while (at < text.length && text[at] in " \t\r\n") at++
        }

        fun value(): Any? {
            space()
            require(at < text.length) { "JSON: ends early" }
            return when (text[at]) {
                '{' -> obj()
                '[' -> list()
                '"' -> string()
                't' -> word("true", true)
                'f' -> word("false", false)
                'n' -> word("null", null)
                else -> number()
            }
        }

        private fun word(word: String, value: Any?): Any? {
            require(text.startsWith(word, at)) { "JSON: unexpected text at $at" }
            at += word.length
            return value
        }

        private fun obj(): Map<String, Any?> {
            val out = LinkedHashMap<String, Any?>()
            at++
            space()
            if (text.getOrNull(at) == '}') return out.also { at++ }
            while (true) {
                space()
                require(text.getOrNull(at) == '"') { "JSON: expected a key at $at" }
                val key = string()
                space()
                require(text.getOrNull(at) == ':') { "JSON: expected : at $at" }
                at++
                out[key] = value()
                space()
                when (text.getOrNull(at)) {
                    ',' -> at++
                    '}' -> return out.also { at++ }
                    else -> throw IllegalArgumentException("JSON: expected , or } at $at")
                }
            }
        }

        private fun list(): List<Any?> {
            val out = ArrayList<Any?>()
            at++
            space()
            if (text.getOrNull(at) == ']') return out.also { at++ }
            while (true) {
                out += value()
                space()
                when (text.getOrNull(at)) {
                    ',' -> at++
                    ']' -> return out.also { at++ }
                    else -> throw IllegalArgumentException("JSON: expected , or ] at $at")
                }
            }
        }

        private fun string(): String {
            val out = StringBuilder()
            at++
            while (true) {
                require(at < text.length) { "JSON: unterminated string" }
                when (val c = text[at++]) {
                    '"' -> return out.toString()
                    '\\' -> {
                        require(at < text.length) { "JSON: unterminated string" }
                        when (val e = text[at++]) {
                            '"', '\\', '/' -> out.append(e)
                            'b' -> out.append('\b')
                            'f' -> out.append('\u000c')
                            'n' -> out.append('\n')
                            'r' -> out.append('\r')
                            't' -> out.append('\t')
                            'u' -> {
                                require(at + 4 <= text.length) { "JSON: short \\u escape" }
                                out.append(text.substring(at, at + 4).toInt(16).toChar())
                                at += 4
                            }
                            else -> throw IllegalArgumentException("JSON: bad escape at $at")
                        }
                    }
                    else -> out.append(c)
                }
            }
        }

        private fun number(): Any {
            val start = at
            while (at < text.length && (text[at].isDigit() || text[at] in "+-.eE")) at++
            val s = text.substring(start, at)
            require(s.isNotEmpty()) { "JSON: unexpected text at $start" }
            return s.toLongOrNull() ?: s.toDoubleOrNull() ?: throw IllegalArgumentException("JSON: bad number $s")
        }
    }
}
