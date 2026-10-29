import com.android.build.api.dsl.ApplicationExtension
import com.android.build.api.variant.ApplicationAndroidComponentsExtension
import org.gradle.api.artifacts.component.ModuleComponentIdentifier
import org.gradle.api.artifacts.result.ResolvedArtifactResult
import org.gradle.maven.MavenModule
import org.gradle.maven.MavenPomArtifact

// AGP 9 compiles Kotlin itself, so there is no org.jetbrains.kotlin.android plugin. The
// Compose compiler plugin's version also sets the Kotlin version AGP uses.
plugins {
    id("com.android.application") version "9.2.1" apply false
    id("com.android.library") version "9.2.1" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.4.21" apply false
}

// One version for every platform: the repo's VERSION file (see the root CMakeLists.txt).
// versionCode packs it as major*100000 + minor*1000 + patch*10; the phone adds 0 and the
// watch 1, since the two share an applicationId and Play wants every code unique.
val kithsongVersion = rootDir.resolve("../VERSION").readText().trim()
val (versionMajor, versionMinor, versionPatch) = kithsongVersion.split(".").map(String::toInt)
extra["kithsongVersionName"] = kithsongVersion
extra["kithsongVersionCode"] = versionMajor * 100000 + versionMinor * 1000 + versionPatch * 10

// The SDK's CMake: :native builds the core with it, and the apps run cmake/gen_strings.cmake
// with it to turn strings/*.json into string resources.
extra["kithsongCmakeVersion"] = "3.30.5"

// The UI strings come from the repo's strings/*.json, shared with the desktop apps (see
// strings/README.md): cmake/gen_strings.cmake writes them out as values*/strings.xml in a
// generated res directory, before resources are merged. Each app module names the sections
// of en.json it takes, and gets R.string/R.plurals for them:
//   extra["kithsongStringSections"] = listOf("shared", "android")
abstract class GenerateStrings : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val stringsDir: DirectoryProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val script: RegularFileProperty

    @get:Input
    abstract val cmake: Property<String>

    @get:Input
    abstract val sections: ListProperty<String>

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @get:Inject
    abstract val execOperations: ExecOperations

    @TaskAction
    fun generate() {
        val out = outputDir.get().asFile
        out.deleteRecursively() // no stale values-<lang> from a removed translation
        out.mkdirs()
        execOperations.exec {
            commandLine(
                cmake.get(),
                "-DIN=${stringsDir.get().file("en.json").asFile}",
                "-DOUT_DIR=$out",
                "-DMODE=android",
                "-DANDROID_SECTIONS=${sections.get().joinToString(",")}",
                "-P", script.get().asFile.path,
            )
        }
    }
}

// The Maven libraries an app ships, for its "Open source licences" screen: one line per module
// of the variant's runtime classpath, "<group>\t<artifact>\t<version>\t<name>\t<licence ids>",
// in assets/licenses/maven.tsv, plus the text of each licence as assets/licenses/maven/<id>.txt
// (from android/licenses/). The licences come from each module's POM, or its parents'. A
// licence the map below doesn't know, or a module with none, fails the build: look the module
// up, and add the licence (and its text, if it's new) or the module (to modules).
abstract class GenerateMavenLicenses : DefaultTask() {
    /** "group:artifact:version\tname\tlicence name|url;..." per module, read from the POMs. */
    @get:Input
    abstract val modules: ListProperty<String>

    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val textsDir: DirectoryProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun generate() {
        val out = outputDir.get().asFile.resolve("licenses")
        out.deleteRecursively()
        out.resolve("maven").mkdirs()
        val used = sortedSetOf<String>()
        val unknown = mutableListOf<String>()
        val lines = modules.get().map { line ->
            val (gav, name, licenses) = line.split("\t")
            val artifact = gav.substringBeforeLast(":")
            val ids = (moduleLicenses[artifact] ?: moduleLicenses[artifact.substringBefore(":")])?.let(::listOf)
                ?: licenses.split(";").filter(String::isNotEmpty).map { license ->
                    licenseId(license) ?: "?".also { unknown += "$gav: $license" }
                }
            if (ids.isEmpty()) unknown += "$gav: no licence in its POM"
            used += ids
            (gav.split(":") + name + ids.joinToString(",")).joinToString("\t")
        }
        if (unknown.isNotEmpty()) {
            throw GradleException("Unknown licences (see GenerateMavenLicenses):\n" + unknown.joinToString("\n"))
        }
        out.resolve("maven.tsv").writeText(lines.sorted().joinToString("\n", postfix = "\n"))
        for (id in used) {
            val text = textsDir.get().file("$id.txt").asFile
            if (!text.isFile) {
                val users = lines.filter { it.substringAfterLast("\t").split(",").contains(id) }
                throw GradleException(
                    "No text for $id: add android/licenses/<id>.txt and map these to it in " +
                        "GenerateMavenLicenses.moduleLicenses:\n" + users.joinToString("\n"),
                )
            }
            text.copyTo(out.resolve("maven/$id.txt"))
        }
    }

    private companion object {
        // A licence's id (its text's file name) from its POM name or URL, "name|url".
        val licenseIds = listOf(
            Regex("""apache.*2\.0|apache-2\.0|LICENSE-2\.0""", RegexOption.IGNORE_CASE) to "Apache-2.0",
            Regex("""\bMIT\b""", RegexOption.IGNORE_CASE) to "MIT",
        )
        fun licenseId(license: String) = licenseIds.firstOrNull { (re, _) -> re.containsMatchIn(license) }?.second

        // Modules whose POMs name no licence, or whose licence text has to carry the project's
        // own copyright line (MIT, BSD): group:artifact, or a whole group, to the text's id.
        // A bare "MIT" has no text, so a new MIT library fails until it gets a line here.
        val moduleLicenses = mapOf(
            "com.materialkolor" to "MIT-MaterialKolor",
            "com.github.ajalt.colormath" to "MIT-Colormath",
        )
    }
}

/**
 * [configuration]'s modules as GenerateMavenLicenses.modules wants them. Resolves the POMs
 * (and their parents, for the licences) through Gradle, so it's lazy and works offline from
 * Gradle's cache once the build has run.
 */
fun Project.mavenLicenseLines(configuration: Configuration): List<String> {
    val ids = configuration.incoming.resolutionResult.allComponents
        .mapNotNull { it.id as? ModuleComponentIdentifier }
        .distinctBy { it.displayName }
    val pomCache = mutableMapOf<String, org.w3c.dom.Element?>()
    fun pom(group: String, artifact: String, version: String): org.w3c.dom.Element? =
        pomCache.getOrPut("$group:$artifact:$version") {
            val result = dependencies.createArtifactResolutionQuery()
                .forModule(group, artifact, version)
                .withArtifacts(MavenModule::class.java, MavenPomArtifact::class.java)
                .execute()
            val file = result.resolvedComponents.firstOrNull()
                ?.getArtifacts(MavenPomArtifact::class.java)
                ?.filterIsInstance<ResolvedArtifactResult>()?.firstOrNull()?.file
            file?.let {
                javax.xml.parsers.DocumentBuilderFactory.newInstance().newDocumentBuilder().parse(it).documentElement
            }
        }
    fun org.w3c.dom.Element.child(name: String) =
        (0 until childNodes.length).map { childNodes.item(it) }
            .firstOrNull { it is org.w3c.dom.Element && it.nodeName == name } as org.w3c.dom.Element?
    fun org.w3c.dom.Element.children(name: String) =
        (0 until childNodes.length).map { childNodes.item(it) }
            .filterIsInstance<org.w3c.dom.Element>().filter { it.nodeName == name }
    fun org.w3c.dom.Element.text(name: String) = child(name)?.textContent?.trim().orEmpty()

    return ids.map { id ->
        var element = pom(id.group, id.module, id.version)
        val name = element?.text("name").orEmpty().ifEmpty { id.module }
        var licenses = emptyList<String>()
        // Up the parent POMs until one names a licence.
        var depth = 0
        while (element != null && depth++ < 5) {
            licenses = element.child("licenses")?.children("license").orEmpty()
                .map { "${it.text("name")}|${it.text("url")}".replace(";", ",").replace("\t", " ") }
            if (licenses.isNotEmpty()) break
            val parent = element.child("parent") ?: break
            element = pom(parent.text("groupId"), parent.text("artifactId"), parent.text("version"))
        }
        "${id.displayName}\t${name.replace("\t", " ")}\t${licenses.joinToString(";")}"
    }
}

subprojects {
    val module = this
    pluginManager.withPlugin("com.android.application") {
        val components = module.extensions.getByType<ApplicationAndroidComponentsExtension>()
        // Lint skips generated files unless told otherwise, and so would miss an unused string
        // of the module's own section. The shared ones are marked tools:ignore="UnusedResources"
        // by the generator, since no one app uses them all.
        module.extensions.configure<ApplicationExtension> {
            lint {
                checkGeneratedSources = true
                lintConfig = rootProject.file("lint.xml")
            }
        }
        // The SDK's CMake (as :native uses), or one on the PATH if that version isn't installed.
        val sdkCmake = components.sdkComponents.sdkDirectory.map { sdk ->
            val exe = if (System.getProperty("os.name").startsWith("Windows")) "cmake.exe" else "cmake"
            val version = rootProject.extra["kithsongCmakeVersion"] as String
            sdk.file("cmake/$version/bin/$exe").asFile.takeIf { it.isFile }?.path ?: "cmake"
        }
        components.onVariants { variant ->
            // Read here, after the module's build script has set it.
            @Suppress("UNCHECKED_CAST")
            val wanted = module.extra.properties["kithsongStringSections"] as List<String>?
                ?: return@onVariants
            val task = module.tasks.register<GenerateStrings>(
                "generate${variant.name.replaceFirstChar(Char::uppercase)}Strings",
            ) {
                stringsDir.set(rootDir.resolve("../strings"))
                script.set(rootDir.resolve("../cmake/gen_strings.cmake"))
                cmake.set(sdkCmake)
                sections.set(wanted)
            }
            variant.sources.res?.addGeneratedSourceDirectory(task, GenerateStrings::outputDir)
        }
        components.onVariants { variant ->
            val task = module.tasks.register<GenerateMavenLicenses>(
                "generate${variant.name.replaceFirstChar(Char::uppercase)}MavenLicenses",
            ) {
                modules.set(module.provider { module.mavenLicenseLines(variant.runtimeConfiguration) })
                textsDir.set(rootDir.resolve("licenses"))
            }
            variant.sources.assets?.addGeneratedSourceDirectory(task, GenerateMavenLicenses::outputDir)
        }
    }
}
