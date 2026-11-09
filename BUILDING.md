# Building Kithsong

The desktop apps build with CMake, and `CMakePresets.json` holds every configuration. Any
editor or IDE that reads presets works: Visual Studio, CLion, Qt Creator, or VS Code with the
CMake Tools extension. The Android and Wear OS apps build with Gradle, which builds the C core
through CMake itself.

Third-party C and C++ libraries (SQLite, TagLib, curl, Mbed TLS, Opus and the rest) are
downloaded at configure time from the URLs and SHA-256 hashes in `third_party/CMakeLists.txt`,
so the first configure needs a network connection. Each build folder keeps its own copy in
`build/<preset>/_deps`.

## Options for every platform

These are optional. Without them the build works, with a warning:

- `KITHSONG_CONTACT`: a URL or email address sent to MusicBrainz in the User-Agent. Their
  terms ask for one, and they may block requests without it.
- `KITHSONG_LASTFM_KEY` and `KITHSONG_LASTFM_SECRET`: a Last.fm API account. Without both,
  scrobbling to Last.fm is off. See `cmake/LastfmKey.cmake` for the other ways to pass them.

Pass them as `-D` options, or put them in a `CMakeUserPresets.json` (gitignored) that
inherits from a preset.

## Windows

You need CMake 3.28 or newer and Ninja on the PATH, for example:

```powershell
winget install Kitware.CMake Ninja-build.Ninja
```

The compiler is [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), at the version pinned
in `cmake/llvm-mingw.pin`. This script downloads it, checks its hash and unpacks it to
`%USERPROFILE%\Tools`, without admin rights. It doesn't need to be on the PATH:

```powershell
scripts\install-llvm-mingw.ps1
```

Set `LLVM_MINGW` to the folder if you keep it somewhere else. Then:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --preset windows-x64
```

The app is `build\windows-x64\desktop\windows\kithsong.exe`. To try it without touching
your own library and settings, `scripts\run-windows-scratch.ps1 <path to kithsong.exe>`
runs it against a throwaway folder in `%TEMP%`.

`windows-arm64` builds for Windows on Arm (from an x64 PC too, but the tests only run on
Arm). Add `-release` to either preset name for an optimised build.

## Linux

You need a C and C++ compiler, CMake 3.28 or newer, Ninja, pkg-config, and the GTK 4.12 and
libadwaita 1.5 development files or newer. Ubuntu 24.04 and Debian 13 have them:

```sh
sudo apt install build-essential cmake ninja-build pkg-config libgtk-4-dev libadwaita-1-dev
```

Fedora:

```sh
sudo dnf install gcc gcc-c++ cmake ninja-build pkgconf-pkg-config gtk4-devel libadwaita-devel
```

Then:

```sh
cmake --preset linux-gtk
cmake --build --preset linux-gtk
ctest --preset linux-gtk
```

The app is `build/linux-gtk/desktop/linux/kithsong`. Use `linux-gtk-release` for an
optimised build. Tests that need an audio device skip themselves when there isn't one.

The releases are built by `scripts/release-linux.sh` (the Flatpak, in the GNOME SDK) and
`scripts/release-linux-tarball.sh` (on Ubuntu 24.04); see `.github/workflows/release.yml`.

## Android and Wear OS

Open the `android/` folder in Android Studio. It needs:

- JDK 21 (Android Studio's own will do)
- Android SDK platform 37
- NDK 27.2.12479018
- CMake 3.30.5 from the SDK Manager (the core needs 3.28 or newer)

Android Studio offers to install what's missing. From the command line, with `JAVA_HOME`
and `ANDROID_HOME` set:

```sh
"$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager" --install "ndk;27.2.12479018" "cmake;3.30.5"
cd android
./gradlew :app:assembleDebug :app:testDebugUnitTest    # gradlew.bat on Windows
./gradlew :wear:assembleDebug
```

`-PkithsongAbis=x86_64` builds only the emulator's ABI, which is much faster. Debug builds use
`ccache` if it's on the PATH; `-PkithsongCcache=off` turns that off.

`android/scripts/android.sh` (Linux) and `android/scripts/android.ps1` (Windows) wrap the
Gradle wrapper and the emulator: `gradle <args>`, `emulator <avd>`, and
`run <avd> <apk> <activity>`.

## Website

See `site/README.md`.
