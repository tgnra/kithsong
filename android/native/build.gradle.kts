// musiccore built with the NDK, plus the JNI bridge and its Kotlin bindings. Shared by the
// phone (:app) and watch (:wear) apps; each app picks the ABIs it ships with abiFilters.
plugins {
    id("com.android.library")
}

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
                // And -PkithsongAcoustidKey=... for fingerprinting; empty turns it off.
                val acoustidKey = (project.findProperty("kithsongAcoustidKey") as String?) ?: ""
                arguments += listOf("-DKITHSONG_ACOUSTID_KEY=$acoustidKey")
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
            version = rootProject.extra["kithsongCmakeVersion"] as String
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
