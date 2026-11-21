#!/usr/bin/env bash
# Builds the GTK app as a Flatpak and bundles it into one file,
# dist/kithsong-<version>-linux-<arch>.flatpak, leaving this architecture's unsigned repo in
# build/flatpak-repo. With `repo`, it instead merges repos like that one, signs the result and
# writes dist/kithsong-flatpak-repo.tar.gz, which the website serves. The release workflow
# builds each architecture on its own runner, then merges them all in one job with the key.
#
#   scripts/release-linux.sh
#   scripts/release-linux.sh repo <repo>...
#
# Needs flatpak and flatpak-builder. The GNOME SDK comes from Flathub (as a --user install)
# if it isn't installed already. Bakes in $KITHSONG_CONTACT,
# $KITHSONG_LASTFM_KEY and $KITHSONG_LASTFM_SECRET when they're set.
#
# `repo` needs flatpak alone, and gpg with $KITHSONG_FLATPAK_GPG_KEY_ID (a key in gpg's
# keyring) to sign; without the key it merges but writes nothing for the website. The plain
# build with the key runs `repo build/flatpak-repo` afterwards, which gives a repo of this
# architecture only: fine to try locally, but the website's needs every architecture.
#
# Install the result with `flatpak install --user <file>`; Flatpak fetches the GNOME runtime
# from Flathub if needed.
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
    echo "The Flatpak builds on Linux only." >&2
    exit 1
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# repo's arguments are relative to where it was run from, so resolve them before the cd.
if [[ "${1:-}" == repo ]]; then
    repos=()
    for repo in "${@:2}"; do repos+=("$(realpath -e -- "$repo")"); done
    set -- repo "${repos[@]}"
fi
cd "$root"
app_id=ng.kithso.Kithsong
flathub=https://dl.flathub.org/repo/flathub.flatpakrepo
branch=stable

# Merges the repos it's given, each holding one build's own refs, into one for kithso.ng/repo
# and signs it. The website's repo is replaced whole each release (see
# .github/workflows/site.yml), so this starts afresh: a client just follows the branch to its
# newest commit, so old ones needn't stay.
site_repo() {
    command -v flatpak >/dev/null || { echo "flatpak isn't installed." >&2; exit 1; }
    if (( $# == 0 )); then
        echo "usage: $0 repo <repo>..." >&2
        exit 1
    fi
    local sign=() site=build/flatpak-site src ref kind id ref_arch ref_branch
    if [[ -n "${KITHSONG_FLATPAK_GPG_KEY_ID:-}" ]]; then
        sign=(--gpg-sign="$KITHSONG_FLATPAK_GPG_KEY_ID")
    fi
    local repo=$site/repo
    local -A seen=()
    rm -rf "$site"
    mkdir -p "$site"
    for src in "$@"; do
        [[ -f "$src/config" && -d "$src/refs/heads" ]] || { echo "$src isn't a Flatpak repo." >&2; exit 1; }
        # The app and its runtime refs (Locale, Debug, when there are any). The appstream
        # branches are left out: build-update-repo writes them again for every architecture.
        local refs=()
        mapfile -t refs < <(cd "$src/refs/heads" && find app runtime -type f 2>/dev/null | sort)
        (( ${#refs[@]} )) || { echo "$src has no app or runtime refs." >&2; exit 1; }
        for ref in "${refs[@]}"; do
            # Two repos with one ref would mean two builds of one architecture.
            [[ -z "${seen[$ref]:-}" ]] || { echo "$ref is in more than one repo." >&2; exit 1; }
            seen[$ref]=1
        done
        # flatpak can't make an empty repo, so the first one is the start of the merged one.
        if [[ ! -d "$repo" ]]; then
            cp -a "$src" "$repo"
            rm -rf "$repo/refs/heads/appstream" "$repo/refs/heads/appstream2" "$repo/summary"* \
                "$repo/summaries" "$repo/tmp" "$repo/.lock"
        else
            flatpak build-commit-from --no-update-summary --src-repo="$src" "$repo" "${refs[@]}"
        fi
    done
    # The builds' commits are unsigned, so sign each ref's here. Then the summary, which lists
    # every architecture's refs; --prune drops the first repo's old appstream commits.
    if (( ${#sign[@]} )); then
        for ref in "${!seen[@]}"; do
            IFS=/ read -r kind id ref_arch ref_branch <<<"$ref"
            local args=(--arch="$ref_arch")
            [[ "$kind" == runtime ]] && args+=(--runtime)
            flatpak build-sign "${args[@]}" "${sign[@]}" "$repo" "$id" "$ref_branch"
        done
    fi
    flatpak build-update-repo "${sign[@]}" --prune --title=Kithsong --default-branch="$branch" "$repo"
    flatpak repo --branches "$repo"
    rm -rf "$repo/tmp" "$repo/.lock"

    if (( ! ${#sign[@]} )); then
        echo "warning: KITHSONG_FLATPAK_GPG_KEY_ID is not set; no signed repo for the website." >&2
        return
    fi
    # What the website serves, unpacked into site/static by .github/workflows/site.yml. The
    # name has no version, so the site can find it in whichever release is the latest. Flatpak
    # picks the architecture from the repo, so one flatpakref does for all of them.
    cat >"$site/kithsong.flatpakref" <<REF
[Flatpak Ref]
Title=Kithsong
Name=$app_id
Branch=$branch
Url=https://kithso.ng/repo/
SuggestRemoteName=kithsong
Homepage=https://kithso.ng
RuntimeRepo=$flathub
IsRuntime=false
GPGKey=$(gpg --export "$KITHSONG_FLATPAK_GPG_KEY_ID" | base64 -w0)
REF
    mkdir -p dist
    tar -czf dist/kithsong-flatpak-repo.tar.gz -C "$site" repo kithsong.flatpakref
    echo "Wrote $root/dist/kithsong-flatpak-repo.tar.gz"
}

case "${1:-}" in
    repo) shift; site_repo "$@"; exit ;;
    '') ;;
    *) echo "usage: $0 [repo <repo>...]" >&2; exit 1 ;;
esac

for tool in flatpak flatpak-builder; do
    command -v "$tool" >/dev/null || { echo "$tool isn't installed." >&2; exit 1; }
done

manifest=packaging/flatpak/$app_id.yml
version="$(head -n1 VERSION | tr -d '[:space:]')"
arch="$(flatpak --default-arch)"
runtime_version="$(sed -n "s/^runtime-version: '\([^']*\)'.*/\1/p" "$manifest")"
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
# This build's repo, unsigned: the bundle comes from it, and `repo` merges it with the other
# architectures' and signs them all.
rm -rf build/flatpak-repo
flatpak build-export build/flatpak-repo build/flatpak-app "$branch"
flatpak build-update-repo --title=Kithsong --default-branch="$branch" build/flatpak-repo

mkdir -p dist
flatpak build-bundle --runtime-repo="$flathub" build/flatpak-repo "$bundle" "$app_id" "$branch"
echo "Wrote $root/$bundle"

if [[ -n "${KITHSONG_FLATPAK_GPG_KEY_ID:-}" ]]; then
    site_repo build/flatpak-repo
fi
