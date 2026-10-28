#!/usr/bin/env bash
# Builds the phone and Wear OS release APKs and AABs into dist/:
#   kithsong-<version>-android.apk/.aab and kithsong-<version>-wearos.apk/.aab, with R8's
#   kithsong-<version>-{android,wearos}-mapping.txt beside them
# The release workflow runs this too; scripts/release-android.ps1 is the Windows twin.
#
#   scripts/release-android.sh
#
# Signs with the keystore at $KITHSONG_KEYSTORE ($KITHSONG_KEYSTORE_PASSWORD,
# $KITHSONG_KEY_ALIAS, $KITHSONG_KEY_PASSWORD); without it the outputs are unsigned. Bakes in
# $KITHSONG_CONTACT, $KITHSONG_ACOUSTID_KEY, $KITHSONG_LASTFM_KEY and $KITHSONG_LASTFM_SECRET
# when they're set. JAVA_HOME and ANDROID_HOME as for android/scripts/android.sh.
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
    echo "warning: KITHSONG_KEYSTORE is not set; the APKs and AABs will be unsigned." >&2
fi
[[ -n "${KITHSONG_CONTACT:-}" ]] ||
    echo "warning: KITHSONG_CONTACT is not set; MusicBrainz may block this build." >&2
[[ -n "${KITHSONG_ACOUSTID_KEY:-}" ]] ||
    echo "warning: KITHSONG_ACOUSTID_KEY is not set; fingerprinting will be off in this build." >&2
[[ -n "${KITHSONG_LASTFM_KEY:-}" && -n "${KITHSONG_LASTFM_SECRET:-}" ]] ||
    echo "warning: KITHSONG_LASTFM_KEY or KITHSONG_LASTFM_SECRET is not set; scrobbling to Last.fm will be off in this build." >&2

"$root/android/scripts/android.sh" gradle --no-daemon \
    :app:assembleRelease :app:bundleRelease :wear:assembleRelease :wear:bundleRelease \
    "-PkithsongContact=${KITHSONG_CONTACT:-}" "-PkithsongAcoustidKey=${KITHSONG_ACOUSTID_KEY:-}" \
    "-PkithsongLastfmKey=${KITHSONG_LASTFM_KEY:-}" "-PkithsongLastfmSecret=${KITHSONG_LASTFM_SECRET:-}"


suffix=$([[ $signed == 1 ]] && echo release || echo release-unsigned)
mkdir -p "$root/dist"
for pair in app:android wear:wearos; do
    module=${pair%%:*} name=${pair##*:}
    out="$root/android/$module/build/outputs"
    cp "$out/apk/release/$module-$suffix.apk" "$root/dist/kithsong-$version-$name.apk"
    cp "$out/bundle/release/$module-release.aab" "$root/dist/kithsong-$version-$name.aab"
    # R8's map back to the real names, for stack traces. The AABs carry it for Play too.
    cp "$out/mapping/release/mapping.txt" "$root/dist/kithsong-$version-$name-mapping.txt"
done

if [[ $signed == 1 ]]; then
    build_tools="$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -n1)"
    for name in android wearos; do
        "$build_tools/apksigner" verify --print-certs "$root/dist/kithsong-$version-$name.apk"
    done
fi
echo "Wrote $root/dist/kithsong-$version-{android,wearos}.{apk,aab}"
