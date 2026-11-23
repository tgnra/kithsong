#!/usr/bin/env bash
# Builds the phone's release APK and AAB into dist/:
#   kithsong-<version>-android.apk/.aab, with R8's kithsong-<version>-android-mapping.txt
#   beside them
# The release workflow runs this too; scripts/release-android.ps1 is the Windows twin.
#
#   scripts/release-android.sh
#
# Signs with the keystore at $KITHSONG_KEYSTORE ($KITHSONG_KEYSTORE_PASSWORD,
# $KITHSONG_KEY_ALIAS, $KITHSONG_KEY_PASSWORD); without it the outputs are unsigned. Bakes in
# $KITHSONG_CONTACT, $KITHSONG_LASTFM_KEY and $KITHSONG_LASTFM_SECRET when they're set. JAVA_HOME and ANDROID_HOME as for android/scripts/android.sh.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(head -n1 "$root/VERSION" | tr -d '[:space:]')"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"

if [[ -n "${KITHSONG_KEYSTORE:-}" ]]; then
    signed=1
    KITHSONG_KEYSTORE="$(cd "$(dirname "$KITHSONG_KEYSTORE")" && pwd)/$(basename "$KITHSONG_KEYSTORE")"
    export KITHSONG_KEYSTORE
else
    signed=0
    echo "warning: KITHSONG_KEYSTORE is not set; the APK and AAB will be unsigned." >&2
fi
[[ -n "${KITHSONG_CONTACT:-}" ]] ||
    echo "warning: KITHSONG_CONTACT is not set; MusicBrainz may block this build." >&2
[[ -n "${KITHSONG_LASTFM_KEY:-}" && -n "${KITHSONG_LASTFM_SECRET:-}" ]] ||
    echo "warning: KITHSONG_LASTFM_KEY or KITHSONG_LASTFM_SECRET is not set; scrobbling to Last.fm will be off in this build." >&2

# Two runs: the AAB carries armeabi-v7a as well, for the store; the APK doesn't (see
# android/app/build.gradle.kts).
for task in :app:bundleRelease :app:assembleRelease; do
    "$root/android/scripts/android.sh" gradle --no-daemon "$task" \
        "-PkithsongContact=${KITHSONG_CONTACT:-}" \
        "-PkithsongLastfmKey=${KITHSONG_LASTFM_KEY:-}" "-PkithsongLastfmSecret=${KITHSONG_LASTFM_SECRET:-}"
done


suffix=$([[ $signed == 1 ]] && echo release || echo release-unsigned)
mkdir -p "$root/dist"
out="$root/android/app/build/outputs"
cp "$out/apk/release/app-$suffix.apk" "$root/dist/kithsong-$version-android.apk"
cp "$out/bundle/release/app-release.aab" "$root/dist/kithsong-$version-android.aab"
# R8's map back to the real names, for stack traces. The AAB carries it for Play too.
cp "$out/mapping/release/mapping.txt" "$root/dist/kithsong-$version-android-mapping.txt"

if [[ $signed == 1 ]]; then
    build_tools="$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -n1)"
    "$build_tools/apksigner" verify --print-certs "$root/dist/kithsong-$version-android.apk"
fi
echo "Wrote $root/dist/kithsong-$version-android.{apk,aab}"
