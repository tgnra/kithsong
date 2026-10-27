# Builds the Windows release and packs it as dist\musicapp-<version>-windows-x64.zip: one
# musicapp-<version>\ folder holding musicapp.exe and LICENSES\. The release workflow runs this
# too.
#
#   scripts\release-windows.ps1
#
# Needs w64devkit's gcc (on PATH, or at $env:W64DEVKIT), CMake >= 3.28 and Ninja. Bakes in
# $env:MUSICAPP_CONTACT and $env:MUSICAPP_ACOUSTID_KEY when they're set, and stamps
# $env:MUSICAPP_BUILD_NUMBER (0-65535) into the file version and the app.
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT') { Write-Error 'The Windows release builds on Windows only.' }

$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content "$root\VERSION" -TotalCount 1).Trim()
$preset = 'windows-w64devkit-release'
$buildDir = "$root\build\$preset"
$stage = "$root\build\stage-windows\musicapp-$version"
$zip = "$root\dist\musicapp-$version-windows-x64.zip"

if ($env:W64DEVKIT) { $env:PATH = "$env:W64DEVKIT\bin;$env:PATH" }
foreach ($tool in 'gcc', 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Error "$tool isn't on PATH. Install w64devkit, CMake and Ninja (or set W64DEVKIT)."
    }
}

# Runs a native command and stops on failure, which $ErrorActionPreference doesn't do.
function Invoke-Checked {
    & $args[0] $args[1..($args.Count - 1)]
    if ($LASTEXITCODE -ne 0) { Write-Error "$($args[0]) failed (exit $LASTEXITCODE)" }
}

$configure = @('--preset', $preset, '-DMUSICAPP_BUILD_TESTS=ON')
if ($env:MUSICAPP_CONTACT) { $configure += "-DMUSICAPP_CONTACT=$env:MUSICAPP_CONTACT" }
else { Write-Warning 'MUSICAPP_CONTACT is not set; MusicBrainz may block this build.' }
if ($env:MUSICAPP_ACOUSTID_KEY) { $configure += "-DMUSICAPP_ACOUSTID_KEY=$env:MUSICAPP_ACOUSTID_KEY" }
else { Write-Warning 'MUSICAPP_ACOUSTID_KEY is not set; fingerprinting will be off in this build.' }
if ($env:MUSICAPP_BUILD_NUMBER) { $configure += "-DMUSICAPP_BUILD_NUMBER=$env:MUSICAPP_BUILD_NUMBER" }

Push-Location $root
try {
    Invoke-Checked cmake @configure
    Invoke-Checked cmake --build --preset $preset
    Invoke-Checked ctest --test-dir $buildDir --output-on-failure
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    Invoke-Checked cmake --install $buildDir --prefix $stage --component musicapp --strip
} finally {
    Pop-Location
}

New-Item -ItemType Directory -Force "$root\dist" | Out-Null
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "Wrote $zip"
