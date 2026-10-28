package ng.kithso.android.ui

import android.content.Context
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import ng.kithso.android.R

/**
 * One third-party library (or, for Android, one project's libraries) and its licence text,
 * the asset at [asset]. [detail] says which licence, or which library groups.
 */
private data class License(val name: String, val detail: String, val asset: String)

private class Licenses(val native: List<License>, val android: List<License>)

/**
 * The licences the build bundles as assets (see android/build.gradle.kts and
 * android/native/build.gradle.kts): licenses/native/index.tsv, "<name>\t<file>\t<licence>" for each C
 * library in the core, beside LICENSES.md; and licenses/maven.tsv, "<group>\t<artifact>\t
 * <version>\t<name>\t<licence ids>" for each Maven module, with licenses/maven/<id>.txt.
 */
private fun Context.loadLicenses(): Licenses {
    fun lines(path: String) = assets.open(path).bufferedReader().readLines().filter(String::isNotBlank)
    val native = lines("licenses/native/index.tsv").map { line ->
        val (name, file, license) = line.split("\t")
        License(name, license, "licenses/native/$file")
    }
    // Modules of one project share a POM name (Compose UI, Media3...): one row each.
    val android = lines("licenses/maven.tsv")
        .map { it.split("\t") }
        .groupBy { (_, _, _, name, ids) -> name to ids }
        .flatMap { (key, modules) ->
            val (name, ids) = key
            val groups = modules.map { it[0] }.distinct().joinToString(", ")
            ids.split(",").map { id ->
                License(name, "$groups · ${spdxName(id)}", "licenses/maven/$id.txt")
            }
        }
        .sortedBy { it.name.lowercase() }
    return Licenses(native, android)
}

// "MIT-MaterialKolor" is the MIT licence with MaterialKolor's copyright line.
private fun spdxName(id: String) = if (id.startsWith("MIT-")) "MIT" else id

/**
 * The third-party libraries in the app, and their licences: the core's C libraries (and
 * LICENSES.md, the overview of them) and the Android libraries. Tapping one calls [onOpen]
 * with its licence text's asset, for [LicenseTextScreen].
 */
@Composable
fun LicensesScreen(bottomPadding: Dp, onOpen: (String) -> Unit, onBack: () -> Unit) {
    val context = LocalContext.current
    val licenses by produceState<Licenses?>(null) { value = withContext(Dispatchers.IO) { context.loadLicenses() } }
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            contentPadding = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
                .add(WindowInsets(bottom = bottomPadding))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "header") { ScreenHeader(stringResource(R.string.licenses), onBack) }
            val loaded = licenses ?: return@LazyColumn
            item(key = "native") { SettingsHeading(stringResource(R.string.licenses_native)) }
            item(key = "overview") {
                ListItem(
                    headlineContent = { Text(stringResource(R.string.licenses_overview)) },
                    supportingContent = { Text("LICENSES.md") },
                    modifier = Modifier.clickable { onOpen(OVERVIEW) },
                )
            }
            items(loaded.native, key = { it.asset }) { LicenseRow(it, onOpen) }
            item(key = "android") { SettingsHeading(stringResource(R.string.licenses_android)) }
            items(loaded.android, key = { it.name + it.asset }) { LicenseRow(it, onOpen) }
        }
    }
}

@Composable
private fun LicenseRow(license: License, onOpen: (String) -> Unit) {
    ListItem(
        headlineContent = { Text(license.name) },
        supportingContent = license.detail.takeIf(String::isNotEmpty)?.let { { Text(it) } },
        modifier = Modifier.clickable { onOpen(license.asset) },
    )
}

private const val OVERVIEW = "licenses/native/LICENSES.md"

/**
 * A licence's text, the asset at [asset]. Paragraphs hard-wrapped at 80 columns are joined up
 * to flow to the screen's width; ones laid out with indents, lists or tables keep their lines,
 * in a monospaced font.
 */
@Composable
fun LicenseTextScreen(asset: String, bottomPadding: Dp, onBack: () -> Unit) {
    val context = LocalContext.current
    val paragraphs by produceState(emptyList<Paragraph>(), asset) {
        value = withContext(Dispatchers.IO) {
            val text = runCatching { context.assets.open(asset).reader().readText() }.getOrDefault("")
            text.replace("\r\n", "\n").split(Regex("\n[ \t]*\n")).filter(String::isNotBlank).map(::paragraph)
        }
    }
    val title by produceState(stringResource(R.string.licenses), asset) {
        if (asset == OVERVIEW) return@produceState
        val all = withContext(Dispatchers.IO) { context.loadLicenses() }
        (all.native + all.android).firstOrNull { it.asset == asset }?.let { value = it.name }
    }
    Surface(color = MaterialTheme.colorScheme.surface, modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            contentPadding = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
                .add(WindowInsets(bottom = bottomPadding + 16.dp))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize(),
        ) {
            item(key = "header") {
                ScreenHeader(if (asset == OVERVIEW) stringResource(R.string.licenses_overview) else title, onBack)
            }
            // A paragraph per item, so a long text isn't laid out all at once.
            items(paragraphs) { paragraph ->
                SelectionContainer {
                    Text(
                        paragraph.text,
                        fontFamily = if (paragraph.laidOut) FontFamily.Monospace else null,
                        style = if (paragraph.laidOut) MaterialTheme.typography.bodySmall else MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.padding(horizontal = 16.dp, vertical = 6.dp),
                    )
                }
            }
        }
    }
}

private class Paragraph(val text: String, val laidOut: Boolean)

// Laid out: lines after the first (which may hang) don't share one indent, or it's a list or
// a table. Otherwise the lines are one run of text.
private fun paragraph(text: String): Paragraph {
    val lines = text.trimEnd().lines().dropWhile(String::isBlank)
    val indent = { line: String -> line.length - line.trimStart().length }
    val marked = lines.any { line -> line.trimStart().let { it.startsWith("|") || it.startsWith("- ") || it.startsWith("* ") } }
    val laidOut = marked || lines.drop(1).map(indent).distinct().size > 1
    return if (laidOut) Paragraph(lines.joinToString("\n").trimIndent(), true)
    else Paragraph(lines.joinToString(" ") { it.trim() }, false)
}
