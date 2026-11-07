# Third-party licenses

Everything below is linked into the shipped binaries. Check this list again whenever a
dependency is added.

| Dependency | Version | License | How it's included |
|------------|---------|---------|-------------------|
| SQLite | 3.47.2 | Public domain | fetched, static |
| yyjson | 0.10.0 | MIT | fetched, static |
| miniaudio | 0.11.21 | Public domain / MIT-0 | fetched, static |
| dr_flac, dr_mp3, dr_wav | 0.13.x / 0.7.x (commit `ea33eed`) | Public domain / MIT-0 | fetched, static |
| stb_image, stb_image_write | 2.30 / 1.16 (commit `f58f558`) | Public domain / MIT | fetched, static |
| miniz | 3.0.2 | MIT | fetched, static |
| mjansson/mdns | 1.4.3 | Public domain (Unlicense) | fetched, header-only |
| Mozilla CA bundle | curl.se 2026-10-20 | MPL-2.0 | fetched, embedded |
| TagLib | 2.0.2 | LGPL-2.1 **or** MPL-1.1 | fetched, static |
| utfcpp | 4.0.6 | BSL-1.0 | fetched, header-only (TagLib dependency) |
| curl | 8.11.1 | curl (MIT-style) | fetched, static |
| Mbed TLS | 3.6.2 | Apache-2.0 | fetched, static |
| libogg | 1.3.6 | BSD-3-Clause | fetched, static |
| libopus | 1.5.2 | BSD-3-Clause (plus royalty-free patent grants) | fetched, static |
| libopusenc | 0.2.1 | BSD-3-Clause | fetched, static (includes the Speex resampler, BSD-3-Clause) |
| opusfile | 0.12 | BSD-3-Clause | fetched, static, without its HTTP support |
| Miller Shuffle (`MillerShuffle_Large`) | Jan 2026 update ([RondeSC/Miller_Shuffle_Algo](https://github.com/RondeSC/Miller_Shuffle_Algo)) | Apache-2.0 | reimplemented in `core/src/playback/shuffle.c` (one function, adapted) |
| material-color-utilities (HCT on CAM16, the Wu and WSMeans quantizers, Score) | Java sources, as Android reaches them through MaterialKolor 5.0.1 ([material-foundation/material-color-utilities](https://github.com/material-foundation/material-color-utilities)) | Apache-2.0 | reimplemented in `core/src/art/art_colors.c` (ported to C, adapted) |

## Obligations

- **TagLib** is statically linked under its **MPL-1.1** option. MPL only requires the
  source of TagLib's own files (including any modifications) to stay available, so static
  linking into a closed or open app is fine. If the LGPL option is used instead, static
  linking requires shipping relinkable object files.
- **Apache-2.0 (Mbed TLS, Miller Shuffle, material-color-utilities), MIT (yyjson), curl, BSL (utfcpp), MPL (CA bundle),
  BSD-3-Clause (libogg, libopus, libopusenc, opusfile)**: include their license texts in an
  "About / Licenses" screen or a `LICENSES` folder in each distributed package.
- **Miller Shuffle** (Copyright 2022 Ronald R. Miller): the repository's LICENSE is
  Apache-2.0, and so is the link in `MillerShuffle.c`'s header, though that header's
  "license:" line still reads "Attribution-NonCommercial-ShareAlike" from an earlier
  version. We go by the LICENSE file. Apache-2.0 asks that changed files say so:
  `shuffle.c` credits the original and lists the changes (its static state worked out per
  call, so it's thread-safe; n = 0 for 2^32 items dropped). The project has no NOTICE file.
- **material-color-utilities** (Copyright 2021-2022 Google LLC): Apache-2.0, with no NOTICE
  file. `art_colors.c` credits it and lists the changes (one C file; WSMeans compares each
  point with every cluster; pixels that aren't opaque left out; a bisection for HCT's
  solver).

## Where the texts ship

- **Desktop packages**: a `LICENSES` folder (Windows) or `share/licenses/` (Linux) with this
  file and each library's own license file, from `cmake/Licenses.cmake`.
- **Android and Wear OS**: the same files as assets, plus the Android libraries' licences. The
  phone shows them under Settings > Open source licences. The Android libraries (AndroidX,
  Kotlin, Media3, Coil, MaterialKolor, ...) aren't listed here: the build reads them off each
  app's dependencies and their POMs (`GenerateMavenLicenses` in `android/build.gradle.kts`),
  with the texts in `android/licenses/`. A library under a licence it doesn't know, or an MIT
  one without its copyright notice there, fails the build.

## Plugins

Plugins ship separately from the app (they're built with `-DKITH_BUILD_PLUGINS=ON`), so what
they bundle isn't in the app's own binaries. Each plugin package carries these texts itself.

### iPod sync (`plugins/ipod`)

| Dependency | Version | License | How it's included |
|------------|---------|---------|-------------------|
| Apple Lossless (ALAC) encoder and decoder | commit `c38887c` ([macosforge/alac](https://github.com/macosforge/alac)) | Apache-2.0 | fetched, static (the decoder only in tests) |
| LAME (libmp3lame) | 3.100 | LGPL-2.0-or-later | fetched, static, without its assembly, vector code and mpglib |
| yyjson | 0.10.0 | MIT | the app's copy, static |
| stb_image | 2.30 (commit `f58f558`) | Public domain / MIT | the app's copy, static |

- **ALAC**: every source file carries an Apache-2.0 header and the repository's LICENSE is
  Apache-2.0. The `codec` folder also holds an old `APPLE_LICENSE.txt` (APSL 2.0) from before
  Apple relicensed it; we go by the file headers. No NOTICE file; no changes to Apple's files
  (the C wrapper is ours, in `src/alac_shim.cpp`).
- **LAME** is under the LGPL, and statically linked. The LGPL asks that users can relink the
  plugin against a changed LAME: the plugin's whole source and build are in this repository,
  which covers that. Ship LAME's `COPYING` with the plugin, and say in its About text that it
  uses LAME. `plugins/ipod/third_party/lame/config.h` is ours (LAME's build normally makes it);
  LAME's own files aren't changed.
- MP3 patents have expired everywhere, so LAME needs no patent licence. ALAC is royalty-free.
