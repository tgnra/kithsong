#!/usr/bin/env bash
# Command-line Android builds and emulator runs, for the VS Code tasks.
#
#   android.sh gradle <args...>            run the Gradle wrapper in android/
#   android.sh emulator <avd>              boot <avd> (if it is not running) and wait for it
#   android.sh run <avd> <apk> <activity>  boot <avd>, install <apk> and start <activity>
#
# Uses $ANDROID_HOME (default ~/Android/Sdk) and $JAVA_HOME. When JAVA_HOME is unset and
# Android Studio from nixpkgs is on PATH, this borrows the environment its launcher sets up:
# the bundled JDK, plus the X11/GL/audio libraries the emulator needs on NixOS.
set -euo pipefail

android_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"

load_studio_env() {
    local launcher wrapper unwrapped
    launcher="$(command -v android-studio)" || return 0
    launcher="$(readlink -f "$launcher")"
    unwrapped="$(grep -o '/nix/store/[^ "]*-android-studio-unwrapped-[^/ "]*' "$launcher" | head -n1)"
    wrapper="$unwrapped/bin/studio"
    [[ -n "$unwrapped" && -f "$wrapper" ]] || return 0
    # The wrapper only exports variables and then execs Studio; run everything but the exec.
    # shellcheck disable=SC1090
    source <(grep -v -e '^#!' -e '^exec ' "$wrapper")
}

if [[ -z "${JAVA_HOME:-}" ]]; then
    load_studio_env
fi
if [[ -z "${JAVA_HOME:-}" ]]; then
    echo "JAVA_HOME is not set and Android Studio was not found on PATH" >&2
    exit 1
fi

adb="$ANDROID_HOME/platform-tools/adb"
emulator="$ANDROID_HOME/emulator/emulator"

# Prints the serial of the running emulator for AVD $1, if there is one.
emulator_serial() {
    local serial
    for serial in $("$adb" devices | awk '/^emulator-/ { print $1 }'); do
        if [[ "$("$adb" -s "$serial" emu avd name 2>/dev/null | head -n1 | tr -d '\r')" == "$1" ]]; then
            echo "$serial"
            return
        fi
    done
}

# Boots AVD $1 unless it is already running, waits for boot to finish, prints its serial.
boot_emulator() {
    local avd="$1" serial log
    serial="$(emulator_serial "$avd")"
    if [[ -z "$serial" ]]; then
        log="${XDG_STATE_HOME:-$HOME/.local/state}/kithsong/emulator-$avd.log"
        mkdir -p "$(dirname "$log")"
        echo "Starting emulator $avd (log: $log)" >&2
        # Detached, so it outlives the task that started it. -no-snapshot cold-boots every time and
        # skips saving a snapshot on exit: restoring Quick Boot snapshots was unreliable.
        setsid "$emulator" -avd "$avd" -no-snapshot -netdelay none -netspeed full >"$log" 2>&1 < /dev/null &
        for _ in $(seq 120); do
            serial="$(emulator_serial "$avd")"
            [[ -n "$serial" ]] && break
            if ! kill -0 $! 2>/dev/null; then
                echo "The emulator exited; see $log" >&2
                exit 1
            fi
            sleep 1
        done
        [[ -n "$serial" ]] || { echo "Timed out waiting for $avd to appear in adb" >&2; exit 1; }
    fi
    echo "Waiting for $avd ($serial) to finish booting" >&2
    "$adb" -s "$serial" wait-for-device
    until [[ "$("$adb" -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == 1 ]]; do
        sleep 1
    done
    echo "$serial"
}

case "${1:-}" in
    gradle)
        shift
        cd "$android_dir"
        exec ./gradlew "$@"
        ;;
    emulator)
        boot_emulator "$2" > /dev/null
        ;;
    run)
        avd="$2" apk="$3" activity="$4"
        serial="$(boot_emulator "$avd")"
        "$adb" -s "$serial" install -r "$apk"
        "$adb" -s "$serial" shell am start -W -n "$activity"
        ;;
    *)
        sed -n '2,6s/^# \?//p' "${BASH_SOURCE[0]}" >&2
        exit 2
        ;;
esac
