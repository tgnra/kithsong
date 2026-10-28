import com.android.build.api.dsl.ApplicationExtension
import com.android.build.api.variant.ApplicationAndroidComponentsExtension

// AGP 9 compiles Kotlin itself, so there is no org.jetbrains.kotlin.android plugin. The
// Compose compiler plugin's version also sets the Kotlin version AGP uses.
plugins {
    id("com.android.application") version "9.2.1" apply false
    id("com.android.library") version "9.2.1" apply false
    id("org.jetbrains.kotlin.plugin.compose") version "2.2.21" apply false
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
    }
}
