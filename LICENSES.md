# Third-party licenses

Everything below is linked into the shipped binaries. Check this list again whenever a
dependency is added.

| Dependency | Version | License | How it's included |
|------------|---------|---------|-------------------|
| SQLite | 3.47.2 | Public domain | fetched, static |
| yyjson | 0.10.0 | MIT | fetched, static |
| miniaudio | 0.11.21 | Public domain / MIT-0 | fetched, static |
| dr_flac, dr_mp3, dr_wav | 0.13.x / 0.7.x (commit `ea33eed`) | Public domain / MIT-0 | fetched, static |
| Mozilla CA bundle | curl.se 2026-10-20 | MPL-2.0 | fetched, embedded |
| TagLib | 2.0.2 | LGPL-2.1 **or** MPL-1.1 | fetched, static |
| utfcpp | 4.0.6 | BSL-1.0 | fetched, header-only (TagLib dependency) |
| curl | 8.11.1 | curl (MIT-style) | fetched, static |
| Mbed TLS | 3.6.2 | Apache-2.0 | fetched, static |

Planned:

| Dependency | License | Notes |
|------------|---------|-------|
| mjansson/mdns | Public domain | mDNS discovery |
| libopus / opusfile | BSD-3-Clause | Opus decode/encode |
| LAME | LGPL-2.0 | MP3 encode; link dynamically or provide relinkable objects |
| Chromaprint | LGPL-2.1 (MIT for parts) | fingerprinting; same LGPL care as LAME |
| noise-c or Monocypher | MIT / BSD-2 or CC0 | pairing and transport encryption |

## Obligations

- **TagLib** is statically linked under its **MPL-1.1** option. MPL only requires the
  source of TagLib's own files (including any modifications) to stay available, so static
  linking into a closed or open app is fine. If the LGPL option is used instead, static
  linking requires shipping relinkable object files.
- **Apache-2.0 (Mbed TLS), MIT (yyjson), curl, BSL (utfcpp), MPL (CA bundle)**: include
  their license texts in an "About / Licenses" screen or a `LICENSES` folder in each
  distributed package.
