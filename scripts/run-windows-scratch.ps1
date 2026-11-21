# Runs the Windows app against a throwaway library and config folder in %TEMP%, so the real
# library and %LOCALAPPDATA%\kithsong are never touched. Both are deleted when the app exits.
# Any arguments (folders or files to import) are passed through to the app.
#
#   run-windows-scratch.ps1 <path to kithsong.exe> [paths...]
param(
  [Parameter(Mandatory)] [string] $Exe,
  [Parameter(ValueFromRemainingArguments)] [string[]] $Paths
)
$ErrorActionPreference = 'Stop'

$root = Join-Path $env:TEMP ("kithsong-scratch-" + [IO.Path]::GetRandomFileName())
$library = Join-Path $root 'library'
$config = Join-Path $root 'config'
New-Item -ItemType Directory -Force $library, $config | Out-Null

# Pre-writing the library location skips the first-run prompt, which offers the Music folder.
# No BOM: WriteAllText's default UTF-8 leaves it out.
$settings = @{ library_dir = $library } | ConvertTo-Json
[IO.File]::WriteAllText((Join-Path $config 'settings.json'), $settings)

Write-Host "Scratch folder: $root"
try {
  $argList = @('--config-dir', "`"$config`"", '--') + ($Paths | ForEach-Object { "`"$_`"" })
  $proc = Start-Process -FilePath $Exe -ArgumentList $argList -PassThru
  $proc.WaitForExit()
  Write-Host "Kithsong exited with code $($proc.ExitCode)"
} finally {
  # The library database can stay locked for a moment after exit.
  for ($i = 0; $i -lt 10 -and (Test-Path $root); $i++) {
    try { Remove-Item -Recurse -Force $root -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
  }
  if (Test-Path $root) { Write-Warning "Could not delete $root" } else { Write-Host "Deleted $root" }
}
