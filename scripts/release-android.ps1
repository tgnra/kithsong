# Builds the phone and Wear OS release APKs and AABs into dist\:
#   musicapp-<version>-android.apk/.aab and musicapp-<version>-wearos.apk/.aab
# The Windows twin of scripts/release-android.sh, which the release workflow runs.
#
#   scripts\release-android.ps1
#
# Signs with the keystore at $env:MUSICAPP_KEYSTORE ($env:MUSICAPP_KEYSTORE_PASSWORD,
# $env:MUSICAPP_KEY_ALIAS, $env:MUSICAPP_KEY_PASSWORD); without it the outputs are unsigned.
# Bakes in $env:MUSICAPP_CONTACT, $env:MUSICAPP_ACOUSTID_KEY, $env:MUSICAPP_LASTFM_KEY and
# $env:MUSICAPP_LASTFM_SECRET when they're set. JAVA_HOME and ANDROID_HOME as for
# android\scripts\android.ps1.
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content "$root\VERSION" -TotalCount 1).Trim()
if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk" }

$signed = [bool]$env:MUSICAPP_KEYSTORE
if ($signed) { $env:MUSICAPP_KEYSTORE = (Resolve-Path $env:MUSICAPP_KEYSTORE).Path }
else { Write-Warning 'MUSICAPP_KEYSTORE is not set; the APKs and AABs will be unsigned.' }
if (-not $env:MUSICAPP_CONTACT) { Write-Warning 'MUSICAPP_CONTACT is not set; MusicBrainz may block this build.' }
if (-not $env:MUSICAPP_ACOUSTID_KEY) { Write-Warning 'MUSICAPP_ACOUSTID_KEY is not set; fingerprinting will be off in this build.' }
if (-not ($env:MUSICAPP_LASTFM_KEY -and $env:MUSICAPP_LASTFM_SECRET)) { Write-Warning 'MUSICAPP_LASTFM_KEY or MUSICAPP_LASTFM_SECRET is not set; scrobbling to Last.fm will be off in this build.' }

# android.ps1 changes directory; keep the caller's.
Push-Location
try {
    & "$root\android\scripts\android.ps1" gradle --no-daemon `
        :app:assembleRelease :app:bundleRelease :wear:assembleRelease :wear:bundleRelease `
        "-PmusicappContact=$env:MUSICAPP_CONTACT" "-PmusicappAcoustidKey=$env:MUSICAPP_ACOUSTID_KEY" `
        "-PmusicappLastfmKey=$env:MUSICAPP_LASTFM_KEY" "-PmusicappLastfmSecret=$env:MUSICAPP_LASTFM_SECRET"

    if ($LASTEXITCODE) { Write-Error "Gradle failed (exit $LASTEXITCODE)" }
} finally {
    Pop-Location
}

$suffix = if ($signed) { 'release' } else { 'release-unsigned' }
New-Item -ItemType Directory -Force "$root\dist" | Out-Null
foreach ($pair in @(@('app', 'android'), @('wear', 'wearos'))) {
    $module, $name = $pair
    $out = "$root\android\$module\build\outputs"
    Copy-Item "$out\apk\release\$module-$suffix.apk" "$root\dist\musicapp-$version-$name.apk"
    Copy-Item "$out\bundle\release\$module-release.aab" "$root\dist\musicapp-$version-$name.aab"
}

if ($signed) {
    $buildTools = Get-ChildItem "$env:ANDROID_HOME\build-tools" -Directory |
        Sort-Object { [version]($_.Name -replace '-.*$', '') } | Select-Object -Last 1
    foreach ($name in 'android', 'wearos') {
        & "$($buildTools.FullName)\apksigner.bat" verify --print-certs "$root\dist\musicapp-$version-$name.apk"
        if ($LASTEXITCODE) { Write-Error "apksigner rejected musicapp-$version-$name.apk" }
    }
}
Write-Host "Wrote $root\dist\musicapp-$version-{android,wearos}.{apk,aab}"
