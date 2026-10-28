# Command-line Android builds and emulator runs on Windows, for the VS Code tasks.
# Same interface as android.sh:
#
#   android.ps1 gradle <args...>            run the Gradle wrapper in android/
#   android.ps1 emulator <avd>              boot <avd> (if it is not running) and wait for it
#   android.ps1 run <avd> <apk> <activity>  boot <avd>, install <apk> and start <activity>
#
# Uses $env:ANDROID_HOME (default %LOCALAPPDATA%\Android\Sdk) and $env:JAVA_HOME (default
# Android Studio's bundled JDK).
$ErrorActionPreference = 'Stop'

$androidDir = Split-Path -Parent $PSScriptRoot
if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk" }
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = "$env:ProgramFiles\Android\Android Studio\jbr" }
if (-not (Test-Path "$env:JAVA_HOME\bin\java.exe")) {
    Write-Error "No JDK at $env:JAVA_HOME; set JAVA_HOME"
}

$adb = "$env:ANDROID_HOME\platform-tools\adb.exe"
$emulator = "$env:ANDROID_HOME\emulator\emulator.exe"

# Returns the serial of the running emulator for AVD $avd, if there is one.
function Get-EmulatorSerial($avd) {
    foreach ($line in & $adb devices) {
        if ($line -match '^(emulator-\d+)\s') {
            $serial = $Matches[1]
            $name = & $adb -s $serial emu avd name 2>$null | Select-Object -First 1
            if ($name -and $name.Trim() -eq $avd) { return $serial }
        }
    }
}

# Boots AVD $avd unless it is already running, waits for boot to finish, returns its serial.
function Start-Emulator($avd) {
    $serial = Get-EmulatorSerial $avd
    if (-not $serial) {
        $logDir = "$env:TEMP\kithsong"
        New-Item -ItemType Directory -Force $logDir | Out-Null
        $log = "$logDir\emulator-$avd.log"
        Write-Host "Starting emulator $avd (log: $log)"
        # Its own process, so it outlives the task that started it. -no-snapshot cold-boots every
        # time and skips saving a snapshot on exit: restoring Quick Boot snapshots was unreliable.
        $proc = Start-Process $emulator -ArgumentList '-avd', $avd, '-no-snapshot', '-netdelay', 'none', '-netspeed', 'full' `
            -RedirectStandardOutput $log -RedirectStandardError "$log.err" -WindowStyle Hidden -PassThru
        for ($i = 0; $i -lt 120 -and -not $serial; $i++) {
            Start-Sleep 1
            $serial = Get-EmulatorSerial $avd
            if (-not $serial -and $proc.HasExited) { Write-Error "The emulator exited; see $log" }
        }
        if (-not $serial) { Write-Error "Timed out waiting for $avd to appear in adb" }
    }
    Write-Host "Waiting for $avd ($serial) to finish booting"
    & $adb -s $serial wait-for-device
    while ((& $adb -s $serial shell getprop sys.boot_completed 2>$null | Out-String).Trim() -ne '1') {
        Start-Sleep 1
    }
    return $serial
}

switch ($args[0]) {
    'gradle' {
        Set-Location $androidDir
        & .\gradlew.bat @($args | Select-Object -Skip 1)
        exit $LASTEXITCODE
    }
    'emulator' {
        Start-Emulator $args[1] | Out-Null
    }
    'run' {
        $serial = Start-Emulator $args[1]
        & $adb -s $serial install -r $args[2]
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
        & $adb -s $serial shell am start -W -n $args[3]
        exit $LASTEXITCODE
    }
    default {
        Get-Content $PSCommandPath | Select-Object -Skip 3 -First 3 |ForEach-Object { $_ -replace '^# ?', '' }
        exit 2
    }
}
