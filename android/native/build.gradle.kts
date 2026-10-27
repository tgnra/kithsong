// musiccore built with the NDK, plus the JNI bridge and its Kotlin bindings. Shared by the
// phone (:app) and watch (:wear) apps; each app picks the ABIs it ships with abiFilters.
plugins {
    id("com.android.library")
}

android {
    namespace = "app.musicapp.nativecore"
    compileSdk = 36
    ndkVersion = "27.2.12479018"

    defaultConfig {
        // getifaddrs() in core/src/discovery needs API 24.
        minSdk = 24

        externalNativeBuild {
            cmake {
                // Pass e.g. -PmusicappContact=... to set the MusicBrainz User-Agent contact.
                val contact = (project.findProperty("musicappContact") as String?) ?: ""
                arguments += listOf("-DMUSICAPP_CONTACT=$contact")
                // And -PmusicappAcoustidKey=... for fingerprinting; empty turns it off.
                val acoustidKey = (project.findProperty("musicappAcoustidKey") as String?) ?: ""
                arguments += listOf("-DMUSICAPP_ACOUSTID_KEY=$acoustidKey")
                // 16 KB page alignment, required by Android 15+ devices; NDK r28 does it by default.
                arguments += listOf("-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON")
            }
        }
        ndk {
            // armeabi-v7a is for watches: many Wear OS devices run a 32-bit userspace.
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            // The core needs CMake >= 3.28; install it with `sdkmanager "cmake;3.30.5"`.
            version = "3.30.5"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
