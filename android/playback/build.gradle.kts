// The core's player as a Media3 Player (CoreMediaPlayer), shared by the phone (:app) and
// watch (:wear) apps: each wraps it in its own MediaSessionService. Only what both need is
// here; queue items, notifications and browsing stay with each app.
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
    // apps add media3-session for the session itself; keep the versions the same.
    api("androidx.media3:media3-common:1.11.1")
}
