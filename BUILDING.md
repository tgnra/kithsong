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

Or all three in one go:

```powershell
cmake --workflow --preset windows-x64
```

`windows-arm64` builds for Windows on Arm (from an x64 PC too, but the tests only run on
Arm). Add `-release` to either preset name for an optimised build.

`windows-x64-vs` and `windows-arm64-vs` are the debug builds plus a `.pdb` beside each exe,
for Visual Studio's debugger (see [IDEs](#ides)). The exes keep their DWARF debug info, so
GDB and LLDB work with them too. The `.pdb`s about double the size of the build folder, which
is why the plain debug presets leave them out. That's the `KITHSONG_CODEVIEW` option; the
release presets never set it.

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

or `cmake --workflow --preset linux-gtk` for all three.

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

## IDEs

Everything an IDE needs is in `CMakePresets.json`, so the repository has no editor settings
(`.vs/`, `.idea/` and `.vscode/` are gitignored). On Windows, install llvm-mingw and Ninja
first, as above: the presets find the compiler themselves, so no IDE needs to be told where
it is. For your own options, such as `KITHSONG_CONTACT`, add a `CMakeUserPresets.json` that
inherits from a preset; every IDE below picks it up too.

Each preset has a workflow of the same name, which configures, builds and runs the tests:

```powershell
cmake --workflow --preset windows-x64           # or windows-x64-release, linux-gtk, ...
cmake --workflow --list-presets
```

The `windows-arm64` workflows stop at the tests on an x64 PC, which can't run Arm builds.

### Visual Studio

Visual Studio 2022 (17.x) or 2026 with the "C++ CMake tools for Windows" component, which
comes with the "Desktop development with C++" workload. It brings its own CMake and Ninja.
MSVC itself isn't used.

1. **File > Open > Folder** and pick the repository's root folder.
2. Choose a preset in the configuration dropdown on the toolbar: "Windows x64 (llvm-mingw,
   with .pdb for Visual Studio)" to debug, or any other to build. The Linux presets are
   hidden on Windows. If Visual Studio offers a `CMakeSettings.json` instead, turn on
   **Tools > Options > CMake > General > Use CMakePresets.json** (it's the default when the
   file exists).
3. **Build > Build All**. The tests show up in Test Explorer.
4. To debug, pick `kithsong.exe` in the startup item dropdown and press F5. The `-vs`
   presets write `kithsong.pdb` beside it, which Visual Studio's debugger reads; it can't
   read the DWARF of the other presets. IntelliSense runs in clang mode
   (`windows-clang-x64` or `-arm64`), from the preset.

C++ standard library types show up raw in the debugger, as Visual Studio has no visualizers
for libc++; the C code is unaffected.

### CLion

1. Add a toolchain in **Settings > Build, Execution, Deployment > Toolchains**: **+ >
   MinGW**, with the llvm-mingw folder as the toolset (for example
   `%USERPROFILE%\Tools\llvm-mingw-20261104-ucrt-x86_64`). Leave CMake as the bundled one,
   and set the build tool to Ninja if CLion doesn't find it. The compiler fields don't matter:
   the presets' toolchain file sets the compiler.
2. Open the repository's root folder. CLion lists the presets as CMake profiles in
   **Settings > Build, Execution, Deployment > CMake**; enable the ones you want, pick the
   llvm-mingw toolchain for each, and switch off CLion's own "Debug" profile.
3. Debug with GDB. llvm-mingw has no GDB, but CLion's bundled GDB reads the DWARF in the
   exes of every debug preset, so CLion doesn't need the `-vs` ones. CLion's bundled LLDB is
   only for its MSVC toolchain. From CLion 2026.2, a DAP debug profile pointing at
   llvm-mingw's `bin\lldb-dap.exe` is another way.

The toolchain's name is your own, so the presets don't name one.

### VS Code

Install the CMake Tools extension (`ms-vscode.cmake-tools`) and, for IntelliSense and
debugging, C/C++ (`ms-vscode.cpptools`), or clangd, which reads
`build/<preset>/compile_commands.json`. CMake Tools uses the presets as they are: **CMake:
Select Configure Preset**, then build with F7 and run the tests from the Testing view.

To debug, add a `.vscode/launch.json` of your own. With C/C++'s Windows debugger, which needs
the `.pdb` of a `-vs` preset:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "kithsong",
      "type": "cppvsdbg",
      "request": "launch",
      "program": "${command:cmake.launchTargetPath}",
      "cwd": "${workspaceFolder}"
    }
  ]
}
```

With the CodeLLDB extension (`vadimcn.vscode-lldb`), use `"type": "lldb"` instead; it reads
the DWARF. Pick the target with **CMake: Set Launch/Debug Target**.

### Android Studio

Open the `android/` folder, not the repository's root. Gradle builds the C core through the
SDK's CMake, so the presets don't come into it. The `app` and `wear` run configurations
appear after the Gradle sync, and native code debugs with Android Studio's own LLDB.

### What has been checked

On Windows x64: the workflows from the command line, with CMake 4.4 and with the CMake 3.31
and Ninja that come with Visual Studio 2022 17.14; that the `.pdb`s match their exes and give
functions, source lines and types to Microsoft's DbgHelp and `dumpbin`; and that GDB and
llvm-mingw's LLDB still debug the same exes from their DWARF. The steps inside the Visual
Studio IDE, CLion and VS Code's debuggers haven't been tried yet. If one of them goes
differently, please say so in an issue.

## Website

See `site/README.md`.
