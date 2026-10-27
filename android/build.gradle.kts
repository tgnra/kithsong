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
val musicappVersion = rootDir.resolve("../VERSION").readText().trim()
val (versionMajor, versionMinor, versionPatch) = musicappVersion.split(".").map(String::toInt)
extra["musicappVersionName"] = musicappVersion
extra["musicappVersionCode"] = versionMajor * 100000 + versionMinor * 1000 + versionPatch * 10

// The SDK's CMake: :native builds the core with it, and :app runs cmake/gen_strings.cmake
// with it to turn strings/*.json into string resources.
extra["musicappCmakeVersion"] = "3.30.5"
