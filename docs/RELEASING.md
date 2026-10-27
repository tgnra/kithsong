# Releasing

Every platform's version comes from the top-level `VERSION` file. CMake, both Android apps and
the release workflow all read it.

| Platform | Package | Script |
|----------|---------|--------|
| Windows | `musicapp-<ver>-windows-x64.zip`, a portable folder with `musicapp.exe` and `LICENSES\` | `scripts/release-windows.ps1` (Windows) |
| Linux | `musicapp-<ver>-linux-x86_64.flatpak`, a single-file Flatpak bundle | `scripts/release-linux.sh` (Linux) |
| Android | `musicapp-<ver>-android.apk` / `.aab` | `scripts/release-android.sh` or `.ps1` |
| Wear OS | `musicapp-<ver>-wearos.apk` / `.aab` | the same script |

Each script writes into `dist/`. The [release workflow](../.github/workflows/release.yml) runs
the same scripts on GitHub Actions.

## Making a release

1. Set `VERSION` to the new version, e.g. `0.2.0`, and commit.
2. Tag and push: `git tag v0.2.0 && git push origin v0.2.0`. Use a tag such as `v0.2.0-rc1`
   for a prerelease. The suffix is ignored when checking the tag against `VERSION`.
3. The workflow builds all four platforms and creates a **draft** release with the six files
   attached. Review it on GitHub and publish it.

To test the builds without a release, run the workflow by hand (Actions → Release → Run
workflow). The files are attached to the run as artifacts.

Windows builds also carry a build number, the 4th field of the exe's file version (e.g.
`0.2.0.57`), shown in the app's sidebar footer. CMake takes it as `MUSICAPP_BUILD_NUMBER`
(0-65535, default 0), and `scripts/release-windows.ps1` passes `$env:MUSICAPP_BUILD_NUMBER`
through. The workflow sets it to the run number (`github.run_number`), since its shallow
checkouts can't count commits. Local builds are build 0 unless you set it.

Android versionCodes are `major*100000 + minor*1000 + patch*10`, plus 0 for the phone and 1
for the watch. The two apps share the `app.musicapp` id, so Play needs every code to be
unique.

## Secrets

Add these under Settings → Secrets and variables → Actions. For local builds, set the same
values as environment variables. Each script warns about any that are missing.

| Secret | Local variable | What it is |
|--------|----------------|------------|
| `MUSICAPP_CONTACT` | `MUSICAPP_CONTACT` | Contact URL or email for the MusicBrainz User-Agent |
| `MUSICAPP_ACOUSTID_KEY` | `MUSICAPP_ACOUSTID_KEY` | AcoustID application key |
| `MUSICAPP_LASTFM_KEY` | `MUSICAPP_LASTFM_KEY` | Last.fm API key, for scrobbling |
| `MUSICAPP_LASTFM_SECRET` | `MUSICAPP_LASTFM_SECRET` | Last.fm API shared secret |

| `ANDROID_KEYSTORE_BASE64` | `MUSICAPP_KEYSTORE` (a path) | The release keystore, base64-encoded for the secret |
| `ANDROID_KEYSTORE_PASSWORD` | `MUSICAPP_KEYSTORE_PASSWORD` | |
| `ANDROID_KEY_ALIAS` | `MUSICAPP_KEY_ALIAS` | |
| `ANDROID_KEY_PASSWORD` | `MUSICAPP_KEY_PASSWORD` | |

Without the keystore, local Android builds come out unsigned. Tagged builds in CI fail
instead.

### The Android keystore

Create it once:

```sh
keytool -genkeypair -v -keystore musicapp-release.jks -keyalg RSA -keysize 4096 \
  -validity 10000 -alias musicapp
base64 -w0 musicapp-release.jks   # PowerShell: [Convert]::ToBase64String([IO.File]::ReadAllBytes("musicapp-release.jks"))
```

Paste the base64 output into `ANDROID_KEYSTORE_BASE64`.

**Back the `.jks` file and its passwords up somewhere safe, outside the repo.** Android
only installs an update signed with the same key, so if the key is lost, every sideloaded
install has to be uninstalled before the next version can go on. The phone and the watch
share the key. If you use Play App Signing, this key becomes your upload key, and Google
holds the app signing key.

## Installing

- **Windows:** unzip anywhere and run `musicapp.exe`. The build isn't code-signed, so
  SmartScreen warns on first launch (More info → Run anyway). To update, replace the folder.
  Settings and the library live elsewhere, so they survive.
- **Linux:** `flatpak install --user musicapp-<ver>-linux-x86_64.flatpak`. This fetches the
  GNOME runtime from Flathub if needed. To update, install the newer bundle. To uninstall,
  run `flatpak uninstall app.musicapp.Musicapp`. The sandbox can reach `~/Music`. Folders
  elsewhere are opened through the file chooser.
- **Android / Wear OS:** `adb install musicapp-<ver>-android.apk`, or open the file on the
  device. For a watch, use `adb -s <watch> install musicapp-<ver>-wearos.apk` over Wi-Fi
  debugging. The `.aab` files are for uploading to Google Play.

## Not done yet

- Windows: no installer, MSIX/Store package or code signing.
- Linux: not on Flathub. Flathub builds offline, so every fetched dependency in
  `third_party/CMakeLists.txt` would need to be a Flatpak source. x86_64 only.
- Android: R8 isn't enabled (`isMinifyEnabled = false`), so the APKs carry all of Compose.
  The third-party license texts aren't in the apps yet.
- The icons in `packaging/icons/` are generated from `musicapp.svg` by `render.py`. Rerun it
  after changing the SVG.
