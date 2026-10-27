plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "app.musicapp"
    compileSdk = 37

    defaultConfig {
        applicationId = "app.musicapp"
        // Android 7.0: Compose needs 23, the native core 24 (see :native).
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
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
