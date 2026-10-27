#!/usr/bin/env bash
# Builds the GTK app as a Flatpak and bundles it into one file,
# dist/musicapp-<version>-linux-<arch>.flatpak. The release workflow runs this too.
#
#   scripts/release-linux.sh
#
# Needs flatpak and flatpak-builder. The GNOME SDK comes from Flathub (as a --user install)
# if it isn't installed already. Bakes in $MUSICAPP_CONTACT, $MUSICAPP_ACOUSTID_KEY,
# $MUSICAPP_LASTFM_KEY and $MUSICAPP_LASTFM_SECRET when they're set.
#
# Install the result with `flatpak install --user <file>`; Flatpak fetches the GNOME runtime
# from Flathub if needed.
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
    echo "The Flatpak builds on Linux only." >&2
    exit 1
fi
for tool in flatpak flatpak-builder; do
    command -v "$tool" >/dev/null || { echo "$tool isn't installed." >&2; exit 1; }
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
app_id=app.musicapp.Musicapp
manifest=packaging/flatpak/$app_id.yml
version="$(head -n1 VERSION | tr -d '[:space:]')"
arch="$(flatpak --default-arch)"
runtime_version="$(sed -n "s/^runtime-version: '\([^']*\)'.*/\1/p" "$manifest")"
flathub=https://dl.flathub.org/repo/flathub.flatpakrepo
bundle="dist/musicapp-$version-linux-$arch.flatpak"

# The manifest's build reads these from an initial-cache file, which is gitignored and
# removed again afterwards.
secrets=packaging/flatpak/secrets.local.cmake
trap 'rm -f "$secrets"' EXIT
cmake_quote() { local s=${1//\\/\\\\}; printf '"%s"' "${s//\"/\\\"}"; }
: >"$secrets"
if [[ -n "${MUSICAPP_CONTACT:-}" ]]; then
    echo "set(MUSICAPP_CONTACT $(cmake_quote "$MUSICAPP_CONTACT") CACHE STRING \"\")" >>"$secrets"
else
    echo "warning: MUSICAPP_CONTACT is not set; MusicBrainz may block this build." >&2
fi
if [[ -n "${MUSICAPP_ACOUSTID_KEY:-}" ]]; then
    echo "set(MUSICAPP_ACOUSTID_KEY $(cmake_quote "$MUSICAPP_ACOUSTID_KEY") CACHE STRING \"\")" >>"$secrets"
else
    echo "warning: MUSICAPP_ACOUSTID_KEY is not set; fingerprinting will be off in this build." >&2
fi
if [[ -n "${MUSICAPP_LASTFM_KEY:-}" && -n "${MUSICAPP_LASTFM_SECRET:-}" ]]; then
    echo "set(MUSICAPP_LASTFM_KEY $(cmake_quote "$MUSICAPP_LASTFM_KEY") CACHE STRING \"\")" >>"$secrets"
    echo "set(MUSICAPP_LASTFM_SECRET $(cmake_quote "$MUSICAPP_LASTFM_SECRET") CACHE STRING \"\")" >>"$secrets"
else
    echo "warning: MUSICAPP_LASTFM_KEY or MUSICAPP_LASTFM_SECRET is not set; scrobbling to Last.fm will be off in this build." >&2
fi


# The CI container has the SDK installed system-wide; elsewhere fetch it for this user.
deps=()
if ! flatpak info "org.gnome.Sdk//$runtime_version" >/dev/null 2>&1; then
    flatpak remote-add --user --if-not-exists flathub "$flathub"
    deps=(--user --install-deps-from=flathub)
fi

flatpak-builder "${deps[@]}" --force-clean --disable-rofiles-fuse \
    --state-dir=build/flatpak-state --repo=build/flatpak-repo \
    build/flatpak-app "$manifest"

if command -v appstreamcli >/dev/null; then
    appstreamcli validate --no-net --explain \
        "build/flatpak-app/files/share/metainfo/$app_id.metainfo.xml"
fi

mkdir -p dist
flatpak build-bundle --runtime-repo="$flathub" build/flatpak-repo "$bundle" "$app_id"
echo "Wrote $root/$bundle"
