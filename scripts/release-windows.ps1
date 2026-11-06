# Builds the Windows release and packs it as dist\kithsong-<version>-windows-<arch>.zip: one
# kithsong-<version>\ folder holding kithsong.exe and LICENSES\. The release workflow runs this
# too.
#
#   scripts\release-windows.ps1 [-Arch x64|arm64]
#
# Runs the tests too, unless this machine can't run what it built (arm64 on an x64 PC).
#
# Needs llvm-mingw (scripts\install-llvm-mingw.ps1), CMake >= 3.28 and Ninja. Bakes in
# $env:KITHSONG_CONTACT, $env:KITHSONG_LASTFM_KEY and $env:KITHSONG_LASTFM_SECRET when
# they're set, and stamps $env:KITHSONG_BUILD_NUMBER (0-65535) into the file version and the
# app.
param(
    [ValidateSet('x64', 'arm64')]
    [string]$Arch = 'x64'
)
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT') { Write-Error 'The Windows release builds on Windows only.' }

$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content "$root\VERSION" -TotalCount 1).Trim()
$preset = "windows-$Arch-release"
$buildDir = "$root\build\$preset"
$stage = "$root\build\stage-windows-$Arch\kithsong-$version"
$zip = "$root\dist\kithsong-$version-windows-$Arch.zip"

foreach ($tool in 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Error "$tool isn't on PATH. Install CMake and Ninja."
    }
}

# Runs a native command and stops on failure, which $ErrorActionPreference doesn't do.
function Invoke-Checked {
    & $args[0] $args[1..($args.Count - 1)]
    if ($LASTEXITCODE -ne 0) { Write-Error "$($args[0]) failed (exit $LASTEXITCODE)" }
}

# An Arm PC runs x64 programs too, but not the other way round. With the tests off, ctest below
# finds none and passes.
$cpu = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE
$runnable = $Arch -eq 'x64' -or $cpu -eq 'ARM64'
if (-not $runnable) { Write-Warning "This $cpu machine can't run an $Arch build, so the tests are left out." }

$configure = @('--preset', $preset, "-DKITHSONG_BUILD_TESTS=$(if ($runnable) { 'ON' } else { 'OFF' })")
if ($env:KITHSONG_CONTACT) { $configure += "-DKITHSONG_CONTACT=$env:KITHSONG_CONTACT" }
else { Write-Warning 'KITHSONG_CONTACT is not set; MusicBrainz may block this build.' }
if ($env:KITHSONG_LASTFM_KEY -and $env:KITHSONG_LASTFM_SECRET) {
    $configure += "-DKITHSONG_LASTFM_KEY=$env:KITHSONG_LASTFM_KEY", "-DKITHSONG_LASTFM_SECRET=$env:KITHSONG_LASTFM_SECRET"
} else { Write-Warning 'KITHSONG_LASTFM_KEY or KITHSONG_LASTFM_SECRET is not set; scrobbling to Last.fm will be off in this build.' }

if ($env:KITHSONG_BUILD_NUMBER) { $configure += "-DKITHSONG_BUILD_NUMBER=$env:KITHSONG_BUILD_NUMBER" }

Push-Location $root
try {
    Invoke-Checked cmake @configure
    # Turning the tests off leaves an earlier run's test list behind.
    if (-not $runnable) { Remove-Item "$buildDir\CTestTestfile.cmake" -ErrorAction SilentlyContinue }
    Invoke-Checked cmake --build --preset $preset
    # test_analysis watches a run grow by polling, which a busy CI runner can miss.
    $ctest = @('--test-dir', $buildDir, '--output-on-failure')
    if ($env:GITHUB_ACTIONS) { $ctest += '-E', '^test_analysis$' }
    Invoke-Checked ctest @ctest
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    Invoke-Checked cmake --install $buildDir --prefix $stage --component kithsong --strip
} finally {
    Pop-Location
}

# What ships is this one exe, so it has to be for the right CPU and need nothing Windows doesn't
# come with: llvm-mingw's runtime DLLs, or the older msvcrt, would mean a wrong link.
$objdump = (Select-String -Path "$buildDir\CMakeCache.txt" -Pattern '^CMAKE_OBJDUMP:FILEPATH=(.+)$').Matches[0].Groups[1].Value
$format = @{ x64 = 'coff-x86-64'; arm64 = 'coff-arm64' }[$Arch]
if (-not ((& $objdump -f "$stage\kithsong.exe") -match "file format $format")) { Write-Error "kithsong.exe isn't $format." }
$foreign = (& $objdump -p "$stage\kithsong.exe") -match 'DLL Name: (lib|msvc|vcruntime)'
if ($foreign) { Write-Error "kithsong.exe needs a DLL it doesn't ship with: $foreign" }

New-Item -ItemType Directory -Force "$root\dist" | Out-Null
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "Wrote $zip"
