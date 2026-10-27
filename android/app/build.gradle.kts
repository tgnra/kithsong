plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "app.musicapp"
    compileSdk = 37
    // The same NDK as :native, which AGP needs to strip the native core's debug symbols.
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "app.musicapp"
        // Android 7.0: Compose needs 23, the native core 24 (see :native).
        minSdk = 24
        targetSdk = 36
        versionCode = rootProject.extra["musicappVersionCode"] as Int
        versionName = rootProject.extra["musicappVersionName"] as String

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    signingConfigs {
        // Release signing comes from the environment (scripts/release-android.*). Without
        // it, release builds come out unsigned. The phone and the watch use the same key.
        System.getenv("MUSICAPP_KEYSTORE")?.let { keystore ->
            create("release") {
                storeFile = file(keystore)
                storePassword = System.getenv("MUSICAPP_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("MUSICAPP_KEY_ALIAS")
                keyPassword = System.getenv("MUSICAPP_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        compose = true
    }
}

// The UI strings come from the repo's strings/*.json, shared with the desktop apps (see
// strings/README.md): cmake/gen_strings.cmake writes them out as values*/strings.xml in a
// generated res directory, before resources are merged.
abstract class GenerateStrings : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val stringsDir: DirectoryProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val script: RegularFileProperty

    @get:Input
    abstract val cmake: Property<String>

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
                "-P", script.get().asFile.path,
            )
        }
    }
}

// The SDK's CMake (as :native uses), or one on the PATH if that version isn't installed.
val sdkCmake = androidComponents.sdkComponents.sdkDirectory.map { sdk ->
    val exe = if (System.getProperty("os.name").startsWith("Windows")) "cmake.exe" else "cmake"
    val version = rootProject.extra["musicappCmakeVersion"] as String
    sdk.file("cmake/$version/bin/$exe").asFile.takeIf { it.isFile }?.path ?: "cmake"
}

androidComponents {
    onVariants { variant ->
        val task = tasks.register<GenerateStrings>("generate${variant.name.replaceFirstChar(Char::uppercase)}Strings") {
            stringsDir.set(rootDir.resolve("../strings"))
            script.set(rootDir.resolve("../cmake/gen_strings.cmake"))
            cmake.set(sdkCmake)
        }
        variant.sources.res?.addGeneratedSourceDirectory(task, GenerateStrings::outputDir)
    }
}

dependencies {
    implementation(project(":native"))

    val composeBom = platform("androidx.compose:compose-bom:2026.09.00")
    implementation(composeBom)
    implementation("androidx.activity:activity-compose:1.13.0")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.ui:ui-tooling-preview")
    // The launch screen, in the system's light or dark colours, on every version back to 24.
    implementation("androidx.core:core-splashscreen:1.2.0")
    // Album art loading and caching; the art itself comes from ui/art/Art.kt's ArtSource.
    implementation("io.coil-kt.coil3:coil-compose:3.4.0")
    // The media session, notification and MediaController around the core's player. Only the
    // session: the core decodes and plays, so there's no ExoPlayer.
    implementation("androidx.media3:media3-session:1.11.1")
    // Colour schemes from album art. 4.1.x is the last built with Kotlin 2.3, which the
    // project's Kotlin 2.2 can still read; 5.x needs 2.4.
    implementation("com.materialkolor:material-kolor:4.1.1")
    // Background sync now and then, on Wi-Fi, while the app isn't on screen (sync/SyncWorker.kt).
    implementation("androidx.work:work-runtime-ktx:2.11.1")
    debugImplementation("androidx.compose.ui:ui-tooling")
}
