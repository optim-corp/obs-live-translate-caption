# Copies a local Windows build into an OBS Studio install (README "Build from source").
# Usage: powershell -ExecutionPolicy Bypass -File scripts\install-dev.ps1 [-ObsDir <path>] [-Config RelWithDebInfo]
# Re-launches itself elevated, because OBS usually lives under Program Files.
param(
    [string]$ObsDir = 'C:\Program Files\obs-studio',
    [string]$Config = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$name = 'obs-live-translate-caption'
$build = Join-Path $repo "build_x64\$Config"

if (Get-Process obs64 -ErrorAction SilentlyContinue) {
    throw 'OBS is running. Close it first: the plugin DLL is locked while OBS is open.'
}
if (-not (Test-Path (Join-Path $build "$name.dll"))) {
    throw "No build found in $build. Run: cmake --build --preset windows-x64-vs2026"
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -ObsDir `"$ObsDir`" -Config $Config"
    $p = Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait -PassThru
    exit $p.ExitCode
}

$binDir = Join-Path $ObsDir 'obs-plugins\64bit'
$dataDir = Join-Path $ObsDir "data\obs-plugins\$name"
New-Item -ItemType Directory -Force $binDir, $dataDir | Out-Null
Copy-Item (Join-Path $build "$name.dll") $binDir -Force
if (Test-Path (Join-Path $build "$name.pdb")) { Copy-Item (Join-Path $build "$name.pdb") $binDir -Force }
Copy-Item (Join-Path $repo 'data\locale') $dataDir -Recurse -Force
Write-Host "Installed $name ($Config) into $ObsDir"
