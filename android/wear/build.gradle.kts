plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.plugin.compose")
}

android {
    namespace = "app.musicapp.wear"
    compileSdk = 37
    // The same NDK as :native, which AGP needs to strip the native core's debug symbols.
    ndkVersion = "27.2.12479018"

    defaultConfig {
        // Same id as the phone app, so the two can talk over the Wear Data Layer once they
        // are signed with the same key. The watch build is told apart by its manifest.
        applicationId = "app.musicapp"
        // Wear OS 3, the oldest version still in use; it covers Wear OS 5 (API 34) and later.
        // The Compose for Wear OS libraries would allow 25, but Wear OS 2 is not worth the cost.
        minSdk = 30
        targetSdk = 36
        versionCode = rootProject.extra["musicappVersionCode"] as Int + 1  // the watch's codes end in 1
        versionName = rootProject.extra["musicappVersionName"] as String

        ndk {
            // armeabi-v7a: many Wear OS watches run a 32-bit userspace.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
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

dependencies {
    implementation(project(":native"))

    val composeBom = platform("androidx.compose:compose-bom:2026.09.00")
    implementation(composeBom)
    implementation("androidx.activity:activity-compose:1.13.0")
    // Compose for Wear OS replaces androidx.compose.material/material3 on the watch.
    implementation("androidx.wear.compose:compose-material3:1.7.1")
    implementation("androidx.wear.compose:compose-foundation:1.7.1")
    // SwipeDismissableNavHost: swipe right to go back, as on every Wear OS app.
    implementation("androidx.wear.compose:compose-navigation:1.7.1")
    implementation("androidx.compose.ui:ui-tooling-preview")
    debugImplementation("androidx.compose.ui:ui-tooling")
}
