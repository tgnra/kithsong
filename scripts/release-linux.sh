#!/usr/bin/env bash
# Builds the GTK app as a Flatpak and bundles it into one file,
# dist/kithsong-<version>-linux-<arch>.flatpak. The release workflow runs this too.
#
#   scripts/release-linux.sh
#
# Needs flatpak and flatpak-builder. The GNOME SDK comes from Flathub (as a --user install)
# if it isn't installed already. Bakes in $KITHSONG_CONTACT,
# $KITHSONG_LASTFM_KEY and $KITHSONG_LASTFM_SECRET when they're set.
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
app_id=app.kithsong.Kithsong
manifest=packaging/flatpak/$app_id.yml
version="$(head -n1 VERSION | tr -d '[:space:]')"
arch="$(flatpak --default-arch)"
runtime_version="$(sed -n "s/^runtime-version: '\([^']*\)'.*/\1/p" "$manifest")"
flathub=https://dl.flathub.org/repo/flathub.flatpakrepo
bundle="dist/kithsong-$version-linux-$arch.flatpak"

# The manifest's build reads these from an initial-cache file, which is gitignored and
# removed again afterwards.
secrets=packaging/flatpak/secrets.local.cmake
trap 'rm -f "$secrets"' EXIT
cmake_quote() { local s=${1//\\/\\\\}; printf '"%s"' "${s//\"/\\\"}"; }
: >"$secrets"
if [[ -n "${KITHSONG_CONTACT:-}" ]]; then
    echo "set(KITHSONG_CONTACT $(cmake_quote "$KITHSONG_CONTACT") CACHE STRING \"\")" >>"$secrets"
else
    echo "warning: KITHSONG_CONTACT is not set; MusicBrainz may block this build." >&2
fi
if [[ -n "${KITHSONG_LASTFM_KEY:-}" && -n "${KITHSONG_LASTFM_SECRET:-}" ]]; then
    echo "set(KITHSONG_LASTFM_KEY $(cmake_quote "$KITHSONG_LASTFM_KEY") CACHE STRING \"\")" >>"$secrets"
    echo "set(KITHSONG_LASTFM_SECRET $(cmake_quote "$KITHSONG_LASTFM_SECRET") CACHE STRING \"\")" >>"$secrets"
else
    echo "warning: KITHSONG_LASTFM_KEY or KITHSONG_LASTFM_SECRET is not set; scrobbling to Last.fm will be off in this build." >&2
fi


# The CI container has the SDK installed system-wide; elsewhere fetch it for this user.
deps=()
if ! flatpak info "org.gnome.Sdk//$runtime_version" >/dev/null 2>&1; then
    flatpak remote-add --user --if-not-exists flathub "$flathub"
    deps=(--user --install-deps-from=flathub)
fi

flatpak-builder "${deps[@]}" --force-clean --disable-rofiles-fuse \
    --state-dir=build/flatpak-state build/flatpak-app "$manifest"

# The manifest turns off flatpak-builder's own compose (see there), so compose the software
# catalog here, outside the sandbox, where flatpak-builder would have put it. Without it the
# bundle still installs; software centres just show less about it.
files=build/flatpak-app/files
if command -v appstreamcli >/dev/null; then
    appstreamcli validate --no-net --explain "$files/share/metainfo/$app_id.metainfo.xml"
    if ! appstreamcli compose --no-color --prefix=/ --origin="$app_id" \
        --result-root="$files" --data-dir="$files/share/app-info/xmls" \
        --icons-dir="$files/share/app-info/icons/flatpak" --hints-dir=build/flatpak-hints \
        --components="$app_id,$app_id.desktop" "$files"; then
        echo "warning: appstreamcli compose failed; the bundle has no software catalog." >&2
        rm -rf "$files/share/app-info"
    fi
else
    echo "warning: appstreamcli isn't installed; the bundle has no software catalog." >&2
fi
flatpak build-export build/flatpak-repo build/flatpak-app

mkdir -p dist
flatpak build-bundle --runtime-repo="$flathub" build/flatpak-repo "$bundle" "$app_id"
echo "Wrote $root/$bundle"
