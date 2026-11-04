#!/usr/bin/env bash
# Builds the GTK app for people who don't want the Flatpak and packs it as
# dist/kithsong-<version>-linux-<arch>.tar.gz: one kithsong-<version>/ folder laid out as an
# install prefix (bin/, share/). The release workflow runs this too.
#
#   scripts/release-linux-tarball.sh
#
# Needs CMake >= 3.28, Ninja, a C and C++ compiler and the GTK 4 and libadwaita headers. Bakes
# in $KITHSONG_CONTACT, $KITHSONG_LASTFM_KEY and $KITHSONG_LASTFM_SECRET when they're set, and
# stamps $KITHSONG_BUILD_NUMBER (0-65535) into the app.
#
# The binary links the build machine's GTK, libadwaita and glibc, so it runs on that system and
# newer ones only. That's why the workflow builds on the oldest system with the libadwaita we
# need, and not in the Flatpak job's GNOME SDK. Everything else is linked in statically.
#
# Run it from where it's unpacked with bin/kithsong. To add it to the app menu:
#
#   tar -xzf kithsong-<version>-linux-<arch>.tar.gz --strip-components=1 -C ~/.local
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
    echo "The tarball builds on Linux only." >&2
    exit 1
fi
for tool in cmake ninja pkg-config; do
    command -v "$tool" >/dev/null || { echo "$tool isn't installed." >&2; exit 1; }
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
version="$(head -n1 VERSION | tr -d '[:space:]')"
arch="$(uname -m)"
build=build/tarball
stage="build/stage-linux/kithsong-$version"
tarball="dist/kithsong-$version-linux-$arch.tar.gz"

configure=(-S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DKITHSONG_BUILD_TESTS=OFF)
if [[ -n "${KITHSONG_CONTACT:-}" ]]; then
    configure+=("-DKITHSONG_CONTACT=$KITHSONG_CONTACT")
else
    echo "warning: KITHSONG_CONTACT is not set; MusicBrainz may block this build." >&2
fi
if [[ -n "${KITHSONG_LASTFM_KEY:-}" && -n "${KITHSONG_LASTFM_SECRET:-}" ]]; then
    configure+=("-DKITHSONG_LASTFM_KEY=$KITHSONG_LASTFM_KEY" "-DKITHSONG_LASTFM_SECRET=$KITHSONG_LASTFM_SECRET")
else
    echo "warning: KITHSONG_LASTFM_KEY or KITHSONG_LASTFM_SECRET is not set; scrobbling to Last.fm will be off in this build." >&2
fi
if [[ -n "${KITHSONG_BUILD_NUMBER:-}" ]]; then
    configure+=("-DKITHSONG_BUILD_NUMBER=$KITHSONG_BUILD_NUMBER")
fi

cmake "${configure[@]}"
cmake --build "$build"
rm -rf "$stage"
cmake --install "$build" --prefix "$stage" --component kithsong --strip

mkdir -p dist
tar -czf "$tarball" --owner=0 --group=0 --numeric-owner -C "$(dirname "$stage")" "kithsong-$version"
echo "Wrote $root/$tarball"
