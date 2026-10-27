#!/usr/bin/env bash
# Builds the phone and Wear OS release APKs and AABs into dist/:
#   musicapp-<version>-android.apk/.aab and musicapp-<version>-wearos.apk/.aab
# The release workflow runs this too; scripts/release-android.ps1 is the Windows twin.
#
#   scripts/release-android.sh
#
# Signs with the keystore at $MUSICAPP_KEYSTORE ($MUSICAPP_KEYSTORE_PASSWORD,
# $MUSICAPP_KEY_ALIAS, $MUSICAPP_KEY_PASSWORD); without it the outputs are unsigned. Bakes in
# $MUSICAPP_CONTACT, $MUSICAPP_ACOUSTID_KEY, $MUSICAPP_LASTFM_KEY and $MUSICAPP_LASTFM_SECRET
# when they're set. JAVA_HOME and ANDROID_HOME as for android/scripts/android.sh.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(head -n1 "$root/VERSION" | tr -d '[:space:]')"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"

if [[ -n "${MUSICAPP_KEYSTORE:-}" ]]; then
    signed=1
    MUSICAPP_KEYSTORE="$(cd "$(dirname "$MUSICAPP_KEYSTORE")" && pwd)/$(basename "$MUSICAPP_KEYSTORE")"
    export MUSICAPP_KEYSTORE
else
    signed=0
    echo "warning: MUSICAPP_KEYSTORE is not set; the APKs and AABs will be unsigned." >&2
fi
[[ -n "${MUSICAPP_CONTACT:-}" ]] ||
    echo "warning: MUSICAPP_CONTACT is not set; MusicBrainz may block this build." >&2
[[ -n "${MUSICAPP_ACOUSTID_KEY:-}" ]] ||
    echo "warning: MUSICAPP_ACOUSTID_KEY is not set; fingerprinting will be off in this build." >&2
[[ -n "${MUSICAPP_LASTFM_KEY:-}" && -n "${MUSICAPP_LASTFM_SECRET:-}" ]] ||
    echo "warning: MUSICAPP_LASTFM_KEY or MUSICAPP_LASTFM_SECRET is not set; scrobbling to Last.fm will be off in this build." >&2

"$root/android/scripts/android.sh" gradle --no-daemon \
    :app:assembleRelease :app:bundleRelease :wear:assembleRelease :wear:bundleRelease \
    "-PmusicappContact=${MUSICAPP_CONTACT:-}" "-PmusicappAcoustidKey=${MUSICAPP_ACOUSTID_KEY:-}" \
    "-PmusicappLastfmKey=${MUSICAPP_LASTFM_KEY:-}" "-PmusicappLastfmSecret=${MUSICAPP_LASTFM_SECRET:-}"


suffix=$([[ $signed == 1 ]] && echo release || echo release-unsigned)
mkdir -p "$root/dist"
for pair in app:android wear:wearos; do
    module=${pair%%:*} name=${pair##*:}
    out="$root/android/$module/build/outputs"
    cp "$out/apk/release/$module-$suffix.apk" "$root/dist/musicapp-$version-$name.apk"
    cp "$out/bundle/release/$module-release.aab" "$root/dist/musicapp-$version-$name.aab"
done

if [[ $signed == 1 ]]; then
    build_tools="$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -n1)"
    for name in android wearos; do
        "$build_tools/apksigner" verify --print-certs "$root/dist/musicapp-$version-$name.apk"
    done
fi
echo "Wrote $root/dist/musicapp-$version-{android,wearos}.{apk,aab}"
