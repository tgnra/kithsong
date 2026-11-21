#!/usr/bin/env bash
# Scroll and memory runs of the GTK app over a stress test library (kithcore/stress.h), for the
# performance workflow (.github/workflows/perf.yml) and for running by hand: the Linux side of
# scripts/perf/ui-windows.ps1.
#
#   scripts/perf/ui-linux.sh <install prefix> <stress library> <out folder> [label]
#
# The prefix is an install of the app (cmake --install <build> --prefix <dir> --component
# kithsong), outside the checkout, as a binary inside one runs as a source run. RUNS (3),
# RUN_START (1: the first run's number), PASSES (5), WIDTH (1280) and HEIGHT (800) can be set.
#
# Each run is a fresh process with the profiler on (KITHSONG_PERF) and KITHSONG_PERF_RUN set, so
# the app runs the scroll test PASSES times over (KITHSONG_PERF_PASSES), saves the report as
# <out>/<label>-<n>.txt and quits. The library is copied fresh for each run, as opening one can
# write to it. XDG_CONFIG_HOME points at a scratch folder whose settings.json names it: the real
# ~/.config/kithsong is never read.
#
# The app draws in a headless sway (pixman, no GPU) of the window's size, which paces frames at
# its output's 60 Hz, under a session bus of its own. Needs sway, and dbus-run-session if the
# app shouldn't reach the desktop's bus.
set -euo pipefail

if [[ -z "${KITHSONG_PERF_BUS:-}" ]] && command -v dbus-run-session >/dev/null; then
    KITHSONG_PERF_BUS=1 exec dbus-run-session -- bash "${BASH_SOURCE[0]}" "$@"
fi

if [[ $# -lt 3 ]]; then
    echo "usage: $0 <install prefix> <stress library> <out folder> [label]" >&2
    exit 2
fi
prefix="$(cd "$1" && pwd)"
library="$(cd "$2" && pwd)"
mkdir -p "$3"
out="$(cd "$3" && pwd)"
label="${4:-head}"
runs="${RUNS:-3}"
first="${RUN_START:-1}"
passes="${PASSES:-5}"
width="${WIDTH:-1280}"
height="${HEIGHT:-800}"
timeout_s="${TIMEOUT:-600}"

command -v sway >/dev/null || { echo "sway isn't installed." >&2; exit 1; }
[[ -x "$prefix/bin/kithsong" ]] || { echo "No $prefix/bin/kithsong." >&2; exit 1; }
[[ -f "$library/kithsong-stress-library.txt" ]] || { echo "$library isn't a stress test library." >&2; exit 1; }

scratch="$(mktemp -d "${TMPDIR:-/tmp}/kithsong-perf.XXXXXX")"
sway_pid=""
app_pid=""
cleanup() {
    [[ -n "$app_pid" ]] && kill -9 "$app_pid" 2>/dev/null || true
    [[ -n "$sway_pid" ]] && kill "$sway_pid" 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$scratch"
}
trap cleanup EXIT

export XDG_RUNTIME_DIR="$scratch/run"
mkdir -m 700 "$XDG_RUNTIME_DIR"
cat >"$scratch/sway.conf" <<EOF
output HEADLESS-1 resolution ${width}x${height} position 0 0 scale 1
default_border none
gaps inner 0
gaps outer 0
seat * hide_cursor 1
EOF
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    sway -c "$scratch/sway.conf" >"$scratch/sway.log" 2>&1 &
sway_pid=$!
display=""
for _ in $(seq 100); do
    display="$(cd "$XDG_RUNTIME_DIR" && ls wayland-* 2>/dev/null | grep -v lock | head -n1 || true)"
    [[ -n "$display" ]] && break
    sleep 0.1
done
if [[ -z "$display" ]]; then
    echo "sway didn't start:" >&2
    cat "$scratch/sway.log" >&2
    exit 1
fi
export WAYLAND_DISPLAY="$display"

for run in $(seq "$first" $((first + runs - 1))); do
    home="$scratch/run-$run"
    mkdir -p "$home/config/kithsong" "$home/data" "$home/cache" "$home/config/gtk-4.0"
    cp -a "$library" "$home/library"
    printf '{"library_dir": "%s", "device_name": "Perf runner", "firewall_dont_ask": true}\n' \
        "$home/library" >"$home/config/kithsong/settings.json"
    # As the store screenshots: GNOME's font and text scale, which GTK can't read here.
    printf '[Settings]\ngtk-font-name=Cantarell 11\ngtk-xft-dpi=98304\n' >"$home/config/gtk-4.0/settings.ini"
    report="$out/$label-$run.txt"
    rm -f "$report"
    start=$SECONDS
    XDG_CONFIG_HOME="$home/config" XDG_DATA_HOME="$home/data" XDG_CACHE_HOME="$home/cache" \
        KITHSONG_PERF=1 KITHSONG_PERF_RUN="$report" KITHSONG_PERF_PASSES="$passes" KITHSONG_THEME=light \
        GDK_BACKEND=wayland GSETTINGS_BACKEND=memory NO_AT_BRIDGE=1 \
        "$prefix/bin/kithsong" --window-size "${width}x${height}" >"$home/app.log" 2>&1 &
    app_pid=$!
    while kill -0 "$app_pid" 2>/dev/null; do
        if (( SECONDS - start > timeout_s )); then
            echo "$label run $run took over $timeout_s s:" >&2
            cat "$home/app.log" >&2
            exit 1
        fi
        sleep 0.25
    done
    wait "$app_pid" || true
    app_pid=""
    if [[ ! -s "$report" ]]; then
        echo "$label run $run ended without a report:" >&2
        cat "$home/app.log" >&2
        exit 1
    fi
    echo "$label run $run: $((SECONDS - start)) s"
    rm -rf "$home"
done
