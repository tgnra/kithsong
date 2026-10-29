plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "ng.kithso.android"
    compileSdk = 37
    // The same NDK as :native, which AGP needs to strip the native core's debug symbols.
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "ng.kithso"
        // Android 7.0: Compose needs 23, the native core 24 (see :native).
        minSdk = 24
        targetSdk = 36
        versionCode = rootProject.extra["kithsongVersionCode"] as Int
        versionName = rootProject.extra["kithsongVersionName"] as String

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    signingConfigs {
        // Release signing comes from the environment (scripts/release-android.*). Without
        // it, release builds come out unsigned. The phone and the watch use the same key.
        System.getenv("KITHSONG_KEYSTORE")?.let { keystore ->
            create("release") {
                storeFile = file(keystore)
                storePassword = System.getenv("KITHSONG_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("KITHSONG_KEY_ALIAS")
                keyPassword = System.getenv("KITHSONG_KEY_PASSWORD")
            }
        }
    }

    buildTypes {
        release {
            // R8: shrinks, optimises and renames the code, then drops unused resources. The
            // JNI bridge's keep rules come with :native (native/consumer-rules.pro); the
            // libraries bring their own. The map back is build/outputs/mapping/release/.
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
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

// R.string/R.plurals from strings/en.json (see GenerateStrings in the root build.gradle.kts).
extra["kithsongStringSections"] = listOf("shared", "android")

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
    implementation("io.coil-kt.coil3:coil-compose:3.6.3")
    // The media session, notification and MediaController around the core's player. Only the
    // session: the core decodes and plays, so there's no ExoPlayer.
    implementation("androidx.media3:media3-session:1.11.1")
    // Colour schemes from album art. 5.x is built with Kotlin 2.4, so it needs the project on
    // 2.4 too (the Compose compiler plugin's version in the root build.gradle.kts).
    implementation("com.materialkolor:material-kolor:5.0.1")
    // Background sync now and then, on Wi-Fi, while the app isn't on screen (sync/SyncWorker.kt).
    implementation("androidx.work:work-runtime-ktx:2.11.1")
    debugImplementation("androidx.compose.ui:ui-tooling")
}
