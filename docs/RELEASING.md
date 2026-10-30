# Releasing

Every platform's version comes from the top-level `VERSION` file. CMake, both Android apps and
the release workflow all read it.

| Platform | Package | Script |
|----------|---------|--------|
| Windows | `kithsong-<ver>-windows-x64.zip`, a portable folder with `kithsong.exe` and `LICENSES\` | `scripts/release-windows.ps1` (Windows) |
| Linux | `kithsong-<ver>-linux-x86_64.flatpak`, a single-file Flatpak bundle | `scripts/release-linux.sh` (Linux) |
| Android | `kithsong-<ver>-android.apk` / `.aab`, and `-android-mapping.txt` | `scripts/release-android.sh` or `.ps1` |
| Wear OS | `kithsong-<ver>-wearos.apk` / `.aab`, and `-wearos-mapping.txt` | the same script |

Each script writes into `dist/`. The [release workflow](../.github/workflows/release.yml) runs
the same scripts on GitHub Actions.

The `-mapping.txt` files are R8's maps from the APKs' shrunk names back to the source's, for
reading a stack trace from a release build (`retrace` in the SDK's cmdline-tools). The AABs
carry the same map for Google Play.

## Setting up the workflow

Do this once per repository. Only the Android key and the API keys need setting up. The
Windows and Linux jobs need nothing beyond the repo. The commands use the
[`gh` CLI](https://cli.github.com/), run from a checkout of the repo. Everything can also be
done in the web UI under Settings → Secrets and variables → Actions → New repository secret.

### 1. Create the Android signing key

`keytool` comes with any JDK:

```sh
mkdir -p ~/keys
keytool -genkeypair -v -keystore ~/Documents/musicapp-release.jks -keyalg RSA -keysize 4096 \
  -validity 10000 -alias musicapp
```

It asks for a keystore password, then for a name, organisation and so on. Those end up in the
certificate, which anyone can read from the APK, so a name or a handle is enough. The keystore
is PKCS12, so the key's password is the same as the keystore's.

Keep the `.jks` outside the repo. `.gitignore` skips `*.jks` and `*.keystore` in case one
lands there anyway.

Note the certificate's SHA-256 fingerprint. You'll check builds against it later:

```sh
keytool -list -v -keystore ~/Documents/musicapp-release.jks -alias musicapp | grep SHA256
```

### 2. Back it up

**Back up the `.jks` file and its password somewhere safe, outside the repo and away from this
machine** (a password manager that takes file attachments works well). Android only installs
an update signed with the same key. If the key is lost, every sideloaded install has to be
uninstalled before the next version can go on. The phone and the watch share the key. If you
use Play App Signing, this key becomes your upload key, and Google holds the app signing key.
A lost upload key can be reset through Play support.

### 3. Try a signed build locally (optional)

This catches a wrong password or alias before CI does:

```sh
export KITHSONG_KEYSTORE=~/keys/kithsong-release.jks
export KITHSONG_KEY_ALIAS=kithsong
read -rs KITHSONG_KEYSTORE_PASSWORD && export KITHSONG_KEYSTORE_PASSWORD
export KITHSONG_KEY_PASSWORD="$KITHSONG_KEYSTORE_PASSWORD"
scripts/release-android.sh
```

The script ends by running `apksigner verify --print-certs` on both APKs. The
`SHA-256 digest` it prints should match the fingerprint from step 1, without the colons.

### 4. Add the secrets

```sh
base64 -w0 ~/keys/kithsong-release.jks | gh secret set ANDROID_KEYSTORE_BASE64
gh secret set ANDROID_KEY_ALIAS --body kithsong
gh secret set ANDROID_KEYSTORE_PASSWORD     # prompts for the value
gh secret set ANDROID_KEY_PASSWORD          # the same password again
```

On macOS, use `base64 -i ~/keys/kithsong-release.jks`. In PowerShell, use
`[Convert]::ToBase64String([IO.File]::ReadAllBytes("kithsong-release.jks")) | gh secret set ANDROID_KEYSTORE_BASE64`.

Then set the API keys. They're baked into every platform's build:

```sh
gh secret set KITHSONG_CONTACT
gh secret set KITHSONG_LASTFM_KEY
gh secret set KITHSONG_LASTFM_SECRET
```

`gh secret list` should now show all seven. The full list:

| Secret | Local variable | What it is |
|--------|----------------|------------|
| `KITHSONG_CONTACT` | `KITHSONG_CONTACT` | Contact URL or email for the MusicBrainz User-Agent |
| `KITHSONG_LASTFM_KEY` | `KITHSONG_LASTFM_KEY` | Last.fm API key, for scrobbling, from <https://www.last.fm/api/account/create> |
| `KITHSONG_LASTFM_SECRET` | `KITHSONG_LASTFM_SECRET` | Last.fm API shared secret |
| `ANDROID_KEYSTORE_BASE64` | `KITHSONG_KEYSTORE` (a path) | The release keystore, base64-encoded |
| `ANDROID_KEYSTORE_PASSWORD` | `KITHSONG_KEYSTORE_PASSWORD` | The keystore password |
| `ANDROID_KEY_ALIAS` | `KITHSONG_KEY_ALIAS` | `kithsong`, unless you picked another alias |
| `ANDROID_KEY_PASSWORD` | `KITHSONG_KEY_PASSWORD` | Same as the keystore password |

A missing API key doesn't stop a build. The scripts warn, and that feature is off in the
build. A missing keystore makes manual runs build unsigned APKs, but tagged runs fail, so a
release can't go out unsigned.

### 5. Do a test run

```sh
gh workflow run release.yml
gh run watch
gh run download --name android --dir /tmp/kithsong-test
```

Check the APK's certificate against step 1 with
`$ANDROID_HOME/build-tools/<ver>/apksigner verify --print-certs /tmp/kithsong-test/*-android.apk`.
The job's log also prints it.

If the Android job fails with "Keystore was tampered with, or password was incorrect", the
password secret is wrong. "No key with alias … found" means the alias is wrong. If it fails to
decode, the base64 was probably wrapped: re-encode with `-w0`.

## Making a release

1. Set `VERSION` to the new version, e.g. `0.2.0`, and commit.
2. Tag and push: `git tag v0.2.0 && git push origin v0.2.0`. Use a tag such as `v0.2.0-rc1`
   for a prerelease. The suffix is ignored when checking the tag against `VERSION`.
3. The workflow builds all four platforms and creates a **draft** release with the eight files
   attached. Review it on GitHub and publish it.

To test the builds without a release, run the workflow by hand (Actions → Release → Run
workflow, or `gh workflow run release.yml`). The files are attached to the run as artifacts.

Windows builds also carry a build number, the 4th field of the exe's file version (e.g.
`0.2.0.57`), shown in the app's About box. CMake takes it as `KITHSONG_BUILD_NUMBER`
(0-65535, default 0), and `scripts/release-windows.ps1` passes `$env:KITHSONG_BUILD_NUMBER`
through. The workflow sets it to the run number (`github.run_number`), since its shallow
checkouts can't count commits. Local builds are build 0 unless you set it.

Android versionCodes are `major*100000 + minor*1000 + patch*10`, plus 0 for the phone and 1
for the watch. The two apps share the `ng.kithso` id, so Play needs every code to be
unique.

## Microsoft Store (MSIX)

`scripts/package-msix.ps1` packs the Windows build as an unsigned
`dist/kithsong-<ver>-windows-x64.msix` for uploading to Partner Center, which signs it on
ingestion. It isn't part of the release workflow. Run it on Windows with the Windows SDK
installed (it finds `makeappx.exe` and `makepri.exe` on `PATH` or under
`Windows Kits\10\bin`), plus what `release-windows.ps1` needs:

```powershell
scripts\package-msix.ps1 -IdentityName 12345Example.Kithsong `
    -Publisher 'CN=01234567-89AB-CDEF-0123-456789ABCDEF' -PublisherDisplayName 'Example'
```

The three values are on Partner Center's Product management → Product identity page
(`Package/Identity/Name`, `Package/Identity/Publisher` and
`Package/Properties/PublisherDisplayName`), and must match exactly, or the upload is refused.
They can also come from `$env:KITHSONG_MSIX_IDENTITY_NAME`, `$env:KITHSONG_MSIX_PUBLISHER` and
`$env:KITHSONG_MSIX_PUBLISHER_NAME`. Left out, they're placeholders: the package still builds,
with a warning, but the Store refuses it.

The script runs `release-windows.ps1` (the same build, tests and zip), or packs an existing
install with `-StageDir build\stage-windows\kithsong-<ver>`. It copies that into
`build\msix\layout`, fills in `packaging/msix/AppxManifest.xml.in`, draws the Start, tile
and Store icons into `Assets\` from `packaging/icons` (the `.ico`'s images for the small
sizes, the PNGs scaled down for the rest), indexes them with `makepri` (`-NoPri` skips that)
and runs `makeappx pack`.

- **Version:** `VERSION` as `a.b.c.0`. The Store keeps the 4th field for itself, so the build
  number isn't in it, and each submission needs a higher `VERSION` than the last.
- **Capabilities:** `runFullTrust` (it's an ordinary Win32 app), `internetClient`
  (MusicBrainz, cover art, Last.fm) and `privateNetworkClientServer` (sync). A full-trust app
  isn't sandboxed by them, but the Store lists them.
- **Firewall:** the manifest declares sync's inbound rule (TCP on any port, UDP 5353, Private
  and Domain profiles), which Windows adds when the package installs and removes on uninstall.
  So the packaged app skips its own firewall check and its elevated helper (`firewall.c`).
- **Settings:** the install folder is read-only, but nothing is written beside the exe:
  settings and `sync.db` are in `%LOCALAPPDATA%\kithsong`. Under MSIX Windows redirects the
  app's new files there to `%LOCALAPPDATA%\Packages\<package family name>\LocalCache\Local\kithsong`,
  which only the packaged app sees and which uninstalling deletes. So a Store install starts
  with its own settings and pairings, not the zip build's, and loses them when uninstalled.
  The library folder itself is the user's and isn't redirected.

To try a package before submitting it, sign a copy with a test certificate whose subject is
the `-Publisher` value (`New-SelfSignedCertificate -Type CodeSigningCert -Subject <publisher>
-CertStoreLocation Cert:\CurrentUser\My`, trusted under Trusted People, then
`signtool sign /fd SHA256 /sha1 <thumbprint> <msix>`) and install it with
`Add-AppxPackage`. Or register the unpacked layout, unsigned, with Developer Mode on:
`Add-AppxPackage -Register build\msix\layout\AppxManifest.xml`.

## Installing

- **Windows:** unzip anywhere and run `kithsong.exe`. The build isn't code-signed, so
  SmartScreen warns on first launch (More info → Run anyway). To update, replace the folder.
  Settings and the library live elsewhere, so they survive. From the Store, see
  [Microsoft Store (MSIX)](#microsoft-store-msix) for where settings go.
- **Linux:** `flatpak install --user kithsong-<ver>-linux-x86_64.flatpak`. This fetches the
  GNOME runtime from Flathub if needed. To update, install the newer bundle. To uninstall,
  run `flatpak uninstall app.kithsong.Kithsong`. The sandbox can reach `~/Music`. Folders
  elsewhere are opened through the file chooser.
- **Android / Wear OS:** `adb install kithsong-<ver>-android.apk`, or open the file on the
  device. For a watch, use `adb -s <watch> install kithsong-<ver>-wearos.apk` over Wi-Fi
  debugging. The `.aab` files are for uploading to Google Play.

## Not done yet

- Windows: no installer or code signing. The MSIX package is built by hand, not by the
  workflow, and is unsigned until the Store signs it.
- Linux: not on Flathub. Flathub builds offline, so every fetched dependency in
  `third_party/CMakeLists.txt` would need to be a Flatpak source. x86_64 only.
- The icons in `packaging/icons/` are generated from `kithsong.svg` by `render.py`. Rerun it
  after changing the SVG.
