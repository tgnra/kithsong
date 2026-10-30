# Packs the Windows release as an unsigned dist\kithsong-<version>-windows-x64.msix for the
# Microsoft Store, which signs it on ingestion. Builds with release-windows.ps1 first, unless
# -StageDir names a folder it already installed (build\stage-windows\kithsong-<version>).
#
#   scripts\package-msix.ps1 -IdentityName 12345Example.Kithsong `
#       -Publisher 'CN=01234567-89AB-CDEF-0123-456789ABCDEF' -PublisherDisplayName 'Example'
#
# The three Identity values must match Partner Center's Product management → Product
# identity page (Package/Identity/Name, Package/Identity/Publisher and
# Package/Properties/PublisherDisplayName). They default to $env:KITHSONG_MSIX_IDENTITY_NAME,
# $env:KITHSONG_MSIX_PUBLISHER and $env:KITHSONG_MSIX_PUBLISHER_NAME, then to placeholders,
# which still pack (for a local look at the package) but which the Store rejects.
#
# Needs the Windows SDK (makeappx.exe and makepri.exe, found on PATH or under Windows Kits\10)
# and, to build, what release-windows.ps1 needs. The manifest is
# packaging\msix\AppxManifest.xml.in; the tile and Start icons are made from
# packaging\icons\kithsong.ico's and kithsong-*.png's images.
param(
    [string]$IdentityName = $(if ($env:KITHSONG_MSIX_IDENTITY_NAME) { $env:KITHSONG_MSIX_IDENTITY_NAME } else { 'PLACEHOLDER.Kithsong' }),
    [string]$Publisher = $(if ($env:KITHSONG_MSIX_PUBLISHER) { $env:KITHSONG_MSIX_PUBLISHER } else { 'CN=00000000-0000-0000-0000-000000000000' }),
    [string]$PublisherDisplayName = $(if ($env:KITHSONG_MSIX_PUBLISHER_NAME) { $env:KITHSONG_MSIX_PUBLISHER_NAME } else { 'PLACEHOLDER' }),
    # A release-windows.ps1 install to pack instead of building.
    [string]$StageDir,
    # Leaves out resources.pri. The scale and targetsize icons then go unused.
    [switch]$NoPri
)
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT') { Write-Error 'The MSIX package builds on Windows only.' }

$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content "$root\VERSION" -TotalCount 1).Trim()
$work = "$root\build\msix"
$layout = "$work\layout"
$msix = "$root\dist\kithsong-$version-windows-x64.msix"

# The Store wants a.b.c.0, each field 0-65535; the 4th is its own, so the build number is
# left out. A suffix such as -rc1 is dropped.
if ($version -notmatch '^(\d+)\.(\d+)\.(\d+)') { Write-Error "VERSION ($version) isn't a.b.c." }
$fields = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
if ($fields | Where-Object { $_ -gt 65535 }) { Write-Error "VERSION ($version) has a field over 65535." }
$msixVersion = ($fields -join '.') + '.0'

if ($IdentityName -notmatch '^[A-Za-z0-9.-]{3,50}$') { Write-Error "IdentityName '$IdentityName' isn't 3-50 letters, digits, dots or dashes." }
if ($Publisher -notmatch '^CN=') { Write-Error "Publisher '$Publisher' isn't a distinguished name (CN=...)." }
if ($IdentityName -like 'PLACEHOLDER*' -or $Publisher -eq 'CN=00000000-0000-0000-0000-000000000000' -or $PublisherDisplayName -eq 'PLACEHOLDER') {
    Write-Warning 'Packing with placeholder Identity values; the Store will reject this package. Pass the ones from Partner Center.'
}

# Runs a native command and stops on failure, which $ErrorActionPreference doesn't do.
function Invoke-Checked {
    & $args[0] $args[1..($args.Count - 1)]
    if ($LASTEXITCODE -ne 0) { Write-Error "$($args[0]) failed (exit $LASTEXITCODE)" }
}

# An SDK tool: on PATH, or in the newest Windows Kits\10\bin\<version>\x64.
function Find-SdkTool($name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    if (Test-Path $kits) {
        $found = Get-ChildItem $kits -Directory | Where-Object { $_.Name -match '^10\.[\d.]+$' } |
            Sort-Object { [version]$_.Name } -Descending |
            ForEach-Object { "$($_.FullName)\x64\$name" } | Where-Object { Test-Path $_ } |
            Select-Object -First 1
        if ($found) { return $found }
    }
    Write-Error "$name isn't on PATH or in $kits. Install the Windows SDK."
}

$makeappx = Find-SdkTool 'makeappx.exe'
$makepri = if ($NoPri) { $null } else { Find-SdkTool 'makepri.exe' }

# --- The app ---

if (-not $StageDir) {
    & "$PSScriptRoot\release-windows.ps1"
    $StageDir = "$root\build\stage-windows\kithsong-$version"
}
if (-not (Test-Path "$StageDir\kithsong.exe")) { Write-Error "$StageDir has no kithsong.exe." }
$StageDir = (Resolve-Path $StageDir).Path

if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $layout | Out-Null
Copy-Item -Recurse "$StageDir\*" $layout

# --- The manifest ---

function ConvertTo-XmlText($s) { [Security.SecurityElement]::Escape($s) }
$values = @{
    IDENTITY_NAME          = $IdentityName
    PUBLISHER              = $Publisher
    PUBLISHER_DISPLAY_NAME = $PublisherDisplayName
    VERSION                = $msixVersion
    DISPLAY_NAME           = 'Kithsong'
    DESCRIPTION            = 'A music library manager and player that keeps your devices in sync over the local network.'
}
$manifest = Get-Content -Raw "$root\packaging\msix\AppxManifest.xml.in"
foreach ($key in $values.Keys) { $manifest = $manifest.Replace("@$key@", (ConvertTo-XmlText $values[$key])) }
if ($manifest -match '@[A-Z_]+@') { Write-Error "AppxManifest.xml.in has an unfilled $($Matches[0])." }
# UTF-8 without a BOM, which Windows PowerShell's Set-Content -Encoding UTF8 would add.
[IO.File]::WriteAllText("$layout\AppxManifest.xml", $manifest, [Text.UTF8Encoding]::new($false))

# --- The icons ---

Add-Type -AssemblyName System.Drawing

# The committed renders of kithsong.svg by size: kithsong-<n>.png and the ico's images, which
# render.py writes as PNGs. Small sizes come from the ico rather than a downscale, so they
# stay sharp.
$sources = @{}
foreach ($png in Get-ChildItem "$root\packaging\icons\kithsong-*.png") {
    if ($png.BaseName -match '-(\d+)$') { $sources[[int]$Matches[1]] = [IO.File]::ReadAllBytes($png.FullName) }
}
$ico = [IO.File]::ReadAllBytes("$root\packaging\icons\kithsong.ico")
for ($i = 0; $i -lt [BitConverter]::ToUInt16($ico, 4); $i++) {
    $entry = 6 + 16 * $i
    $size = if ($ico[$entry] -eq 0) { 256 } else { [int]$ico[$entry] }
    $length = [BitConverter]::ToInt32($ico, $entry + 8)
    $offset = [BitConverter]::ToInt32($ico, $entry + 12)
    if (-not $sources.ContainsKey($size)) { $sources[$size] = [byte[]]$ico[$offset..($offset + $length - 1)] }
}

# Writes Assets\<name>.png, width x height, transparent, with the icon iconSize square in the
# middle: drawn from the render that size, or else scaled down from the next bigger one.
function New-Asset($name, $width, $height, $iconSize) {
    $from = $sources.Keys | Where-Object { $_ -ge $iconSize } | Sort-Object | Select-Object -First 1
    if (-not $from) { $from = $sources.Keys | Sort-Object | Select-Object -Last 1 }
    $stream = [IO.MemoryStream]::new($sources[$from])
    $icon = [Drawing.Image]::FromStream($stream)
    $bitmap = [Drawing.Bitmap]::new($width, $height, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bitmap)
    $edges = [Drawing.Imaging.ImageAttributes]::new()
    try {
        $g.Clear([Drawing.Color]::Transparent)
        $g.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
        $edges.SetWrapMode([Drawing.Drawing2D.WrapMode]::TileFlipXY) # no faded border
        $x = [int][Math]::Floor(($width - $iconSize) / 2)
        $y = [int][Math]::Floor(($height - $iconSize) / 2)
        $g.DrawImage($icon, [Drawing.Rectangle]::new($x, $y, $iconSize, $iconSize),
            0, 0, $icon.Width, $icon.Height, [Drawing.GraphicsUnit]::Pixel, $edges)
        $bitmap.Save("$layout\Assets\$name.png", [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $edges.Dispose(); $g.Dispose(); $bitmap.Dispose(); $icon.Dispose(); $stream.Dispose()
    }
}

New-Item -ItemType Directory -Force "$layout\Assets" | Out-Null
# The unqualified files are the scale-100 ones, which makeappx checks for and which Windows
# falls back on without resources.pri. Tiles hold the icon at 2/3 of their height, as
# Windows' own do; the app list, taskbar and Store icons fill theirs, as the icon has its own
# margin.
foreach ($scale in 100, 200) {
    $suffix = if ($scale -eq 100) { '' } else { ".scale-$scale" }
    $k = $scale / 100
    New-Asset "Square44x44Logo$suffix" (44 * $k) (44 * $k) (44 * $k)
    New-Asset "Square150x150Logo$suffix" (150 * $k) (150 * $k) (100 * $k)
    New-Asset "Wide310x150Logo$suffix" (310 * $k) (150 * $k) (100 * $k)
    New-Asset "StoreLogo$suffix" (50 * $k) (50 * $k) (50 * $k)
}
# The taskbar, title bar and Explorer pick these exact sizes. Unplated: drawn as they are, with
# no accent-colour plate behind them.
foreach ($size in 16, 24, 32, 48, 256) {
    New-Asset "Square44x44Logo.targetsize-$size" $size $size $size
    New-Asset "Square44x44Logo.targetsize-${size}_altform-unplated" $size $size $size
}

# --- Packing ---

if ($makepri) {
    # Indexes the icons' scale and targetsize variants, so Windows can pick between them. The
    # config lives outside the layout, or it would be packed too.
    Invoke-Checked $makepri createconfig /cf "$work\priconfig.xml" /dq en-US /pv 10.0.0 /o
    Invoke-Checked $makepri new /pr $layout /cf "$work\priconfig.xml" /mn "$layout\AppxManifest.xml" /of "$layout\resources.pri" /o
}

New-Item -ItemType Directory -Force "$root\dist" | Out-Null
if (Test-Path $msix) { Remove-Item -Force $msix }
Invoke-Checked $makeappx pack /d $layout /p $msix /o
Write-Host "Wrote $msix ($IdentityName $msixVersion, unsigned)"
