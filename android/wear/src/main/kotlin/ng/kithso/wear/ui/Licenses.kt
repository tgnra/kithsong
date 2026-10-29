package ng.kithso.wear.ui

import android.content.Context
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.wear.compose.foundation.lazy.TransformingLazyColumnItemScope
import androidx.wear.compose.foundation.lazy.items
import androidx.wear.compose.material3.FilledTonalButton
import androidx.wear.compose.material3.ListHeader
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.Text
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import ng.kithso.wear.R

// The phone's licences screen (android/app's ui/Licenses.kt), laid out for the watch. The two
// apps share no UI module, so the asset reading is repeated here.

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
    // Modules of one project share a POM name (Compose UI, Wear Compose...): one row each.
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

private const val OVERVIEW = "licenses/native/LICENSES.md"

/**
 * The third-party libraries in the watch app, and their licences: the core's C libraries (and
 * LICENSES.md, the overview of them) and the Android libraries. Tapping one calls [onOpen]
 * with its licence text's asset, for [LicenseTextScreen].
 */
@Composable
fun LicensesScreen(onOpen: (String) -> Unit) {
    val context = LocalContext.current
    val licenses by produceState<Licenses?>(null) { value = withContext(Dispatchers.IO) { context.loadLicenses() } }
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(stringResource(R.string.watch_licenses))
            }
        }
        val loaded = licenses ?: return@ListScreen
        item {
            ListHeader(modifier = transform.modifier(this).fillMaxWidth(), transformation = transform.surface(this)) {
                Text(stringResource(R.string.watch_licenses_native), style = MaterialTheme.typography.titleSmall)
            }
        }
        item {
            FilledTonalButton(
                onClick = { onOpen(OVERVIEW) },
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
                label = { Text(stringResource(R.string.watch_licenses_overview)) },
                secondaryLabel = { Text("LICENSES.md") },
            )
        }
        items(loaded.native, key = { it.asset }) { license ->
            LicenseButton(license, transform, this, onOpen)
        }
        item {
            ListHeader(modifier = transform.modifier(this).fillMaxWidth(), transformation = transform.surface(this)) {
                Text(stringResource(R.string.watch_licenses_android), style = MaterialTheme.typography.titleSmall)
            }
        }
        items(loaded.android, key = { it.name + it.asset }) { license ->
            LicenseButton(license, transform, this, onOpen)
        }
    }
}

@Composable
private fun LicenseButton(
    license: License,
    transform: ItemTransform,
    scope: TransformingLazyColumnItemScope,
    onOpen: (String) -> Unit,
) {
    FilledTonalButton(
        onClick = { onOpen(license.asset) },
        modifier = transform.modifier(scope).fillMaxWidth(),
        transformation = transform.surface(scope),
        label = { Text(license.name, maxLines = 2, overflow = TextOverflow.Ellipsis) },
        secondaryLabel = license.detail.takeIf(String::isNotEmpty)?.let {
            { Text(it, maxLines = 2, overflow = TextOverflow.Ellipsis) }
        },
    )
}

/**
 * A licence's text, the asset at [asset]. Paragraphs hard-wrapped at 80 columns are joined up
 * to flow to the screen's width; ones laid out with indents, lists or tables keep their lines,
 * in a monospaced font.
 */
@Composable
fun LicenseTextScreen(asset: String) {
    val context = LocalContext.current
    val paragraphs by produceState(emptyList<Paragraph>(), asset) {
        value = withContext(Dispatchers.IO) {
            val text = runCatching { context.assets.open(asset).reader().readText() }.getOrDefault("")
            text.replace("\r\n", "\n").split(Regex("\n[ \t]*\n")).filter(String::isNotBlank).map(::paragraph)
        }
    }
    val overview = stringResource(R.string.watch_licenses_overview)
    val title by produceState(if (asset == OVERVIEW) overview else "", asset) {
        if (asset == OVERVIEW) return@produceState
        val all = withContext(Dispatchers.IO) { context.loadLicenses() }
        (all.native + all.android).firstOrNull { it.asset == asset }?.let { value = it.name }
    }
    ListScreen { transform ->
        item {
            ListHeader(
                modifier = transform.modifier(this).fillMaxWidth(),
                transformation = transform.surface(this),
            ) {
                Text(title, maxLines = 2, overflow = TextOverflow.Ellipsis)
            }
        }
        // A paragraph per item, so a long text isn't laid out all at once.
        items(paragraphs) { paragraph ->
            Text(
                paragraph.text,
                fontFamily = if (paragraph.laidOut) FontFamily.Monospace else null,
                style = if (paragraph.laidOut) MaterialTheme.typography.bodyExtraSmall else MaterialTheme.typography.bodySmall,
                modifier = transform.modifier(this).fillMaxWidth().padding(horizontal = 4.dp, vertical = 4.dp),
            )
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
