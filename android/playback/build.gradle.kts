// The core's player as a Media3 Player (CoreMediaPlayer), which the phone app (:app) wraps
// in its MediaSessionService. Queue items, notifications and browsing stay with the app.
plugins {
    id("com.android.library")
}

android {
    namespace = "ng.kithso.playback"
    compileSdk = 37

    defaultConfig {
        // As :native, which it drives.
        minSdk = 24
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    api(project(":native"))
    // Player, SimpleBasePlayer, Timeline and the audio focus and becoming-noisy helpers. The
    // app adds media3-session for the session itself; keep the versions the same.
    api("androidx.media3:media3-common:1.11.1")
}
