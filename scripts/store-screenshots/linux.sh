#!/usr/bin/env bash
# Store screenshots of the GTK app, from the demo library (kithcore/demo.h): each page in light
# and dark, as PNGs of a window filling a headless sway output of a fixed size.
#
#   scripts/store-screenshots/linux.sh [out folder]
#
# Takes the tarball's install (scripts/release-linux-tarball.sh leaves it in
# build/stage-linux/kithsong-<version>/), or the folder in $KITHSONG_STAGE. It's copied out of
# the checkout first, as a binary inside one runs as a source run, with its own settings folder
# and temp library. Each shot is a fresh run: XDG_CONFIG_HOME points at a scratch folder whose
# settings.json names the device and turns off the firewall prompt, and --demo-library makes
# the library there. The real ~/.config/kithsong is never read.
#
# Needs sway and grim (sway --headless draws with pixman, no GPU), and fonts-cantarell and
# adwaita-icon-theme for the app to look as it does on GNOME. WIDTH, HEIGHT and SHOTS (a space
# separated list of home albums album artist search import review sync) can be set.
#
# As on Windows (windows.ps1): import is the import page part-way through a made-up import,
# review the same with the review of the album that needs a decision open, and sync a made-up
# phone's page part-way through a sync with a selection as Opus. Each has a track paused
# part-way in the now-playing strip: Copperline's first at 0:21 in light, Nightjar Season's at
# 0:08 in dark (--demo-playing). See kithcore/demo.h.
set -euo pipefail

# Under a session bus of its own where there's dbus-run-session, so the app can't reach the
# desktop's (a CI runner has none). The whole script, not each run: killing a wrapper would
# leave the app running.
if [[ -z "${KITHSONG_SHOTS_BUS:-}" ]] && command -v dbus-run-session >/dev/null; then
    KITHSONG_SHOTS_BUS=1 exec dbus-run-session -- bash "${BASH_SOURCE[0]}" "$@"
fi

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
out="${1:-out/linux}"
width="${WIDTH:-1600}"
height="${HEIGHT:-1000}"
shots=(${SHOTS:-home albums album artist search import review sync})
default_device="Alice's laptop" # not inside ${...:-}, where bash reads the ' as a quote
device_name="${DEVICE_NAME:-$default_device}"

for tool in sway swaymsg grim sha256sum; do
    command -v "$tool" >/dev/null || { echo "$tool isn't installed." >&2; exit 1; }
done
stage="${KITHSONG_STAGE:-}"
if [[ -z "$stage" ]]; then
    stage="$(ls -d "$root"/build/stage-linux/kithsong-*/ 2>/dev/null | head -n1 || true)"
fi
if [[ -z "$stage" || ! -x "$stage/bin/kithsong" ]]; then
    echo "No build/stage-linux/kithsong-*/bin/kithsong. Run scripts/release-linux-tarball.sh first, or set KITHSONG_STAGE." >&2
    exit 1
fi

page_of() {
    case "$1" in
        home) echo home ;;
        albums) echo albums ;;
        album) echo "album:Copperline" ;;
        artist) echo "artist:Ivy Sunderholt" ;;
        search) echo "search:night" ;;
        import) echo import ;;
        review) echo import-review ;;
        sync) echo sync ;;
        *) echo "Unknown shot $1; one of home albums album artist search import review sync." >&2; return 1 ;;
    esac
}

# The track paused in the now-playing strip, by theme (--demo-playing).
playing_of() {
    case "$1" in
        light) echo "Copperline#1@0:21" ;;
        dark) echo "Nightjar Season#1@0:08" ;;
    esac
}

scratch="$(mktemp -d "${TMPDIR:-/tmp}/kithsong-shots.XXXXXX")"
sway_pid=""
app_pid=""
cleanup() {
    [[ -n "$app_pid" ]] && kill "$app_pid" 2>/dev/null || true
    [[ -n "$sway_pid" ]] && kill "$sway_pid" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$scratch"
}
trap cleanup EXIT

# Out of the checkout, so it runs as an installed copy would.
cp -a "$stage" "$scratch/app"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

# One headless output the size of the shots, the app's window tiled over all of it: no
# borders, gaps, bar or cursor, and GTK draws a tiled window square, without its shadow.
export XDG_RUNTIME_DIR="$scratch/run"
mkdir -m 700 "$XDG_RUNTIME_DIR"
cat >"$scratch/sway.conf" <<EOF
output HEADLESS-1 resolution ${width}x${height} position 0 0 scale 1
default_border none
default_floating_border none
gaps inner 0
gaps outer 0
seat * hide_cursor 1
EOF
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    sway -c "$scratch/sway.conf" >"$scratch/sway.log" 2>&1 &
sway_pid=$!
for _ in $(seq 100); do
    sock="$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -n1 || true)"
    display="$(cd "$XDG_RUNTIME_DIR" && ls wayland-* 2>/dev/null | grep -v lock | head -n1 || true)"
    [[ -n "$sock" && -n "$display" ]] && break
    sleep 0.1
done
if [[ -z "${sock:-}" || -z "${display:-}" ]]; then
    echo "sway didn't start:" >&2
    cat "$scratch/sway.log" >&2
    exit 1
fi
export SWAYSOCK="$sock" WAYLAND_DISPLAY="$display"
outputs="$(swaymsg -t get_outputs)"
[[ "$outputs" == *"\"width\": $width,"* ]] || echo "warning: the output isn't $width wide: $outputs" >&2

# Whether the app's window is up. Not `swaymsg | grep -q`: grep stopping early can fail
# swaymsg with SIGPIPE, and pipefail then makes a match look like none.
has_window() {
    local tree
    tree="$(swaymsg -t get_tree)"
    [[ "$tree" == *'"app_id": "ng.kithso.Kithsong"'* ]]
}

for theme in light dark; do
    for shot in "${shots[@]}"; do
        page="$(page_of "$shot")"
        home="$scratch/$shot-$theme"
        mkdir -p "$home/config/kithsong" "$home/data" "$home/cache"
        printf '{"device_name": "%s", "firewall_dont_ask": true}\n' "$device_name" >"$home/config/kithsong/settings.json"
        # GNOME's font: with no desktop settings to read, GTK would take fontconfig's Sans. And
        # its text scale, 96 dpi in 1024ths: unset, it reads as -1, and libadwaita's widgets
        # sized in sp (AdwClamp: the now-playing strip, the import page, the review) shrink to
        # their least.
        mkdir -p "$home/config/gtk-4.0"
        printf '[Settings]\ngtk-font-name=Cantarell 11\ngtk-xft-dpi=98304\n' >"$home/config/gtk-4.0/settings.ini"
        # Each shot shows one person's devices (Alice's here), except this one: Mallory's
        # laptop lurks among the unpaired devices.
        stranger=""
        [[ "$shot" == sync && "$theme" == dark ]] && stranger="Mallory's laptop"
        XDG_CONFIG_HOME="$home/config" XDG_DATA_HOME="$home/data" XDG_CACHE_HOME="$home/cache" \
            KITHSONG_DEMO_STRANGER="$stranger" KITHSONG_THEME="$theme" GDK_BACKEND=wayland GSETTINGS_BACKEND=memory NO_AT_BRIDGE=1 \
            "$scratch/app/bin/kithsong" --demo-library --window-size "${width}x${height}" \
            --page "$page" --demo-playing "$(playing_of "$theme")" >"$home/app.log" 2>&1 &
        app_pid=$!
        shown=""
        for _ in $(seq 300); do
            if ! kill -0 "$app_pid" 2>/dev/null; then
                echo "Kithsong exited before its window showed ($shot, $theme):" >&2
                cat "$home/app.log" >&2
                exit 1
            fi
            if has_window; then
                shown=1
                break
            fi
            sleep 0.1
        done
        if [[ -z "$shown" ]]; then
            echo "No window after 30 s ($shot, $theme):" >&2
            cat "$home/app.log" >&2
            exit 1
        fi
        # Settled once two captures a second apart are the same: covers load, the page opens.
        sleep 2
        file="$out/linux-$shot-$theme.png"
        last=""
        for _ in $(seq 15); do
            grim "$file"
            hash="$(sha256sum "$file" | cut -d' ' -f1)"
            [[ "$hash" == "$last" ]] && break
            last="$hash"
            sleep 1
        done
        echo "$file"
        # Gone before the next starts: else its window shares the output, and the next one
        # finds it on the network as another device.
        kill "$app_pid" 2>/dev/null || true
        for _ in $(seq 50); do
            kill -0 "$app_pid" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "$app_pid" 2>/dev/null || true
        wait "$app_pid" 2>/dev/null || true
        app_pid=""
        for _ in $(seq 50); do
            has_window || break
            sleep 0.1
        done
        if has_window; then
            echo "The window is still there after the app was stopped ($shot, $theme)." >&2
            exit 1
        fi
    done
done
