# Builds the phone and Wear OS release APKs and AABs into dist\:
#   kithsong-<version>-android.apk/.aab and kithsong-<version>-wearos.apk/.aab, with R8's
#   kithsong-<version>-{android,wearos}-mapping.txt beside them
# The Windows twin of scripts/release-android.sh, which the release workflow runs.
#
#   scripts\release-android.ps1
#
# Signs with the keystore at $env:KITHSONG_KEYSTORE ($env:KITHSONG_KEYSTORE_PASSWORD,
# $env:KITHSONG_KEY_ALIAS, $env:KITHSONG_KEY_PASSWORD); without it the outputs are unsigned.
# Bakes in $env:KITHSONG_CONTACT, $env:KITHSONG_ACOUSTID_KEY, $env:KITHSONG_LASTFM_KEY and
# $env:KITHSONG_LASTFM_SECRET when they're set. JAVA_HOME and ANDROID_HOME as for
# android\scripts\android.ps1.
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content "$root\VERSION" -TotalCount 1).Trim()
if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk" }

$signed = [bool]$env:KITHSONG_KEYSTORE
if ($signed) { $env:KITHSONG_KEYSTORE = (Resolve-Path $env:KITHSONG_KEYSTORE).Path }
else { Write-Warning 'KITHSONG_KEYSTORE is not set; the APKs and AABs will be unsigned.' }
if (-not $env:KITHSONG_CONTACT) { Write-Warning 'KITHSONG_CONTACT is not set; MusicBrainz may block this build.' }
if (-not $env:KITHSONG_ACOUSTID_KEY) { Write-Warning 'KITHSONG_ACOUSTID_KEY is not set; fingerprinting will be off in this build.' }
if (-not ($env:KITHSONG_LASTFM_KEY -and $env:KITHSONG_LASTFM_SECRET)) { Write-Warning 'KITHSONG_LASTFM_KEY or KITHSONG_LASTFM_SECRET is not set; scrobbling to Last.fm will be off in this build.' }

# android.ps1 changes directory; keep the caller's.
Push-Location
try {
    & "$root\android\scripts\android.ps1" gradle --no-daemon `
        :app:assembleRelease :app:bundleRelease :wear:assembleRelease :wear:bundleRelease `
        "-PkithsongContact=$env:KITHSONG_CONTACT" "-PkithsongAcoustidKey=$env:KITHSONG_ACOUSTID_KEY" `
        "-PkithsongLastfmKey=$env:KITHSONG_LASTFM_KEY" "-PkithsongLastfmSecret=$env:KITHSONG_LASTFM_SECRET"

    if ($LASTEXITCODE) { Write-Error "Gradle failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}

$suffix = if ($signed) { 'release' } else { 'release-unsigned' }
New-Item -ItemType Directory -Force "$root\dist" | Out-Null
foreach ($pair in @(@('app', 'android'), @('wear', 'wearos'))) {
    $module, $name = $pair
    $out = "$root\android\$module\build\outputs"
    Copy-Item "$out\apk\release\$module-$suffix.apk" "$root\dist\kithsong-$version-$name.apk"
    Copy-Item "$out\bundle\release\$module-release.aab" "$root\dist\kithsong-$version-$name.aab"
    # R8's map back to the real names, for stack traces. The AABs carry it for Play too.
    Copy-Item "$out\mapping\release\mapping.txt" "$root\dist\kithsong-$version-$name-mapping.txt"
}

if ($signed) {
    $buildTools = Get-ChildItem "$env:ANDROID_HOME\build-tools" -Directory |
        Sort-Object { [version]($_.Name -replace '-.*$', '') } | Select-Object -Last 1
    foreach ($name in 'android', 'wearos') {
        & "$($buildTools.FullName)\apksigner.bat" verify --print-certs "$root\dist\kithsong-$version-$name.apk"
        if ($LASTEXITCODE) { Write-Error "apksigner rejected kithsong-$version-$name.apk" }
    }
}
Write-Host "Wrote $root\dist\kithsong-$version-{android,wearos}.{apk,aab}"
