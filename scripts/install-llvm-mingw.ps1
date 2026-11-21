# Downloads the llvm-mingw release pinned in cmake/llvm-mingw.pin and unpacks it under -Dir,
# in a folder named after the release, so a bumped pin never builds with the old toolchain.
# The zip is portable: no installer, no admin rights, nothing outside that folder changes.
# It needn't be on PATH, as cmake/llvm-mingw.cmake finds it by itself. Prints the folder.
#
#   scripts\install-llvm-mingw.ps1 [-Dir <parent folder>]
#
# Unpacks the build for this machine's own CPU; either one compiles for both x86_64 and aarch64.
param(
    [string]$Dir = "$HOME\Tools"
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest's progress bar makes it far slower

$root = Split-Path -Parent $PSScriptRoot
$pin = Get-Content "$root\cmake\llvm-mingw.pin"
$release = $pin[0].Trim()

# The registry gives the real CPU even if this shell runs under x64 emulation.
$cpu = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Environment').PROCESSOR_ARCHITECTURE
$arch = switch ($cpu) { 'AMD64' { 'x86_64' } 'ARM64' { 'aarch64' } default { Write-Error "No llvm-mingw for a $cpu CPU." } }
$sha256 = ($pin | Where-Object { $_ -like "$arch *" }).Split(' ')[1]

$name = "llvm-mingw-$release-ucrt-$arch"
$target = Join-Path $Dir $name
if (Test-Path "$target\bin\clang.exe") {
    Write-Output $target
    return
}

# Each attempt gets its own new folder, next to the target so the move at the end is a rename:
# an interrupted run never leaves a half-unpacked toolchain under the final name.
New-Item -ItemType Directory -Force $Dir | Out-Null
$work = Join-Path $Dir ("$name.tmp-" + [IO.Path]::GetRandomFileName())
New-Item -ItemType Directory $work | Out-Null
try {
    $zip = "$work\$name.zip"
    Invoke-WebRequest "https://github.com/mstorsjo/llvm-mingw/releases/download/$release/$name.zip" -OutFile $zip
    $actual = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($actual -ne $sha256) { Write-Error "Checksum mismatch for $name.zip: got $actual, want $sha256." }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, "$work\unpacked")
    Move-Item "$work\unpacked\$name" $target
} finally {
    Remove-Item -Recurse -Force $work
}
Write-Output $target
