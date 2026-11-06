// kithcore built with the NDK, plus the JNI bridge and its Kotlin bindings. Shared by the
// phone (:app) and watch (:wear) apps; each app picks the ABIs it ships with abiFilters.
plugins {
    id("com.android.library")
}

// Where CMake copies the fetched libraries' license files, one folder per ABI (see
// cmake/Licenses.cmake's kithsong_copy_licenses). They're the same for every ABI.
val licensesDir = layout.buildDirectory.dir("intermediates/kithsong-licenses")

// What this run was asked to build, as ABIs and ccache depend on it.
val requestedTasks = gradle.startParameter.taskNames
val releaseRun = requestedTasks.any { it.contains("release", ignoreCase = true) }

// The ABIs to compile, each a full build of the core and its libraries. This module is shared,
// so it can't take the apps' abiFilters: without this a phone build compiles armeabi-v7a too,
// which only the watch ships. Anything not named as :app's alone, such as a bare assembleDebug
// or an IDE sync, gets them all. -PkithsongAbis=x86_64 narrows it, for the emulator.
val abiOverride = (findProperty("kithsongAbis") as String?)?.split(",")?.map(String::trim)?.filter(String::isNotEmpty)
if (abiOverride != null && releaseRun) {
    throw GradleException("-PkithsongAbis is for development: a release has to carry every ABI")
}
val phoneOnly = requestedTasks.isNotEmpty() && requestedTasks.all { it.removePrefix(":").startsWith("app:") }
val nativeAbis = abiOverride ?: listOf("arm64-v8a", "x86_64") + if (phoneOnly) emptyList() else listOf("armeabi-v7a")

// ccache, if it's on the PATH, for everything but a release; -PkithsongCcache=off goes without.
// base_dir and hash_dir let worktrees share results, whose paths differ only above the repo, at
// the price of debug info naming the worktree that compiled first.
val ccache = System.getenv("PATH").orEmpty().split(File.pathSeparator)
    .flatMap { listOf(File(it, "ccache.exe"), File(it, "ccache")) }
    .firstOrNull { it.isFile }
    ?.takeUnless { releaseRun || findProperty("kithsongCcache") == "off" }

android {
    namespace = "ng.kithso.core"
    compileSdk = 36
    ndkVersion = "27.2.12479018"

    defaultConfig {
        // getifaddrs() in core/src/discovery needs API 24.
        minSdk = 24

        externalNativeBuild {
            cmake {
                // Pass e.g. -PkithsongContact=... to set the MusicBrainz User-Agent contact.
                val contact = (project.findProperty("kithsongContact") as String?) ?: ""
                arguments += listOf("-DKITHSONG_CONTACT=$contact")
                // And -PkithsongLastfmKey=... -PkithsongLastfmSecret=... for scrobbling to Last.fm.
                // Without them CMake tries $KITHSONG_LASTFM_KEY/_SECRET, then the file
                // -PkithsongLastfmFile=... names (default: last.fm.api at the top of the repo);
                // see cmake/LastfmKey.cmake. The core keeps them obscured, never as plain text.
                val lastfmKey = (project.findProperty("kithsongLastfmKey") as String?) ?: ""
                val lastfmSecret = (project.findProperty("kithsongLastfmSecret") as String?) ?: ""
                arguments += listOf("-DKITHSONG_LASTFM_KEY=$lastfmKey", "-DKITHSONG_LASTFM_SECRET=$lastfmSecret")
                (project.findProperty("kithsongLastfmFile") as String?)?.let {
                    arguments += "-DKITHSONG_LASTFM_FILE=${file(it).absolutePath.replace('\\', '/')}"
                }

                // The native libraries' license texts, copied at configure time for the assets below.
                arguments += "-DKITHSONG_LICENSES_DIR=" + licensesDir.get().asFile.absolutePath.replace('\\', '/')

                // 16 KB page alignment, required by Android 15+ devices; NDK r28 does it by default.
                arguments += listOf("-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON")

                ccache?.let {
                    val repo = rootDir.parentFile.absolutePath.replace('\\', '/')
                    val launcher = it.absolutePath.replace('\\', '/') + ";base_dir=$repo;hash_dir=false"
                    arguments += listOf("-DCMAKE_C_COMPILER_LAUNCHER=$launcher", "-DCMAKE_CXX_COMPILER_LAUNCHER=$launcher")
                }
            }
        }
        ndk {
            // armeabi-v7a is for watches: many Wear OS devices run a 32-bit userspace.
            abiFilters += nativeAbis
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            // The core needs CMake >= 3.28; install it with `sdkmanager "cmake;3.30.5"`.
            version = rootProject.extra["kithsongCmakeVersion"] as String
        }
    }

    // R8: the classes and members the JNI bridge looks up by name. Applied to the apps.
    defaultConfig.consumerProguardFiles("consumer-rules.pro")

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// The native licences as assets/licenses/native/ (index.tsv, LICENSES.md and the texts), for
// the apps' "Open source licences" screens. Merged into each app that depends on :native.
abstract class NativeLicenseAssets : DefaultTask() {
    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val licenses: DirectoryProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @TaskAction
    fun copy() {
        val out = outputDir.get().asFile.resolve("licenses/native")
        out.deleteRecursively()
        val abi = licenses.get().asFile.listFiles { f -> f.resolve("index.tsv").isFile }?.minOrNull()
            ?: throw GradleException("No native license texts in ${licenses.get()}; did CMake run?")
        abi.copyRecursively(out)
    }
}

androidComponents.onVariants { variant ->
    val name = variant.name.replaceFirstChar(Char::uppercase)
    val task = tasks.register<NativeLicenseAssets>("generate${name}NativeLicenseAssets") {
        licenses.set(licensesDir)
        // CMake writes them when it configures, which the native build does first.
        dependsOn(tasks.matching { it.name == "externalNativeBuild$name" })
    }
    variant.sources.assets?.addGeneratedSourceDirectory(task, NativeLicenseAssets::outputDir)
}
