#requires -Version 5
<#
.SYNOPSIS
  Headless Tracy CPU profile capture for the X4 station builder.
.DESCRIPTION
  Drives the full-tracy editor in --profile mode (an orbiting, input-free render of
  a real plan), records a Tracy trace via the CLI capture tool, and exports the
  per-zone CPU statistics to CSV. The whole flow is deterministic and needs no GUI,
  so it is reproducible and scriptable. Open the .tracy in tracy-profiler.exe for
  the flame graph; read the .csv for the zone breakdown without a GUI.
.EXAMPLE
  tools/profile-capture.ps1 -Plan "C:\path\to\Sphere Shipyard.xml" -Frames 400 -Name sphere
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string]$Plan,
  [int]$Frames = 400,
  [string]$Name = 'capture'
)
$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $PSScriptRoot          # repo root (tools/ sits under it)
$tracy  = Join-Path $PSScriptRoot 'tracy'
$editor = Join-Path $root 'build/full-tracy/bin/x4sb-editor.exe'
$outDir = Join-Path $root 'profiling'
$trace  = Join-Path $outDir "$Name.tracy"
$csv    = Join-Path $outDir "$Name.csv"

if (-not (Test-Path $editor)) { throw "editor not built: $editor  (run: cmake --build --preset full-tracy)" }
if (-not (Test-Path $Plan))   { throw "plan not found: $Plan" }
if (-not (Test-Path (Join-Path $tracy 'tracy-capture.exe'))) {
  throw "Tracy CLI tools missing under $tracy  (see tools/tracy/README.md to fetch them)"
}
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# Capture connects to the client (127.0.0.1) and retries until it appears, so start
# it FIRST; -f overwrites any old trace at this path.
$cap = Start-Process -FilePath (Join-Path $tracy 'tracy-capture.exe') `
  -ArgumentList @('-o', $trace, '-f') -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds 500

# TRACY_NO_EXIT makes the editor block at shutdown until the capture has drained
# every queued frame — without it the tail frames are lost on a fast headless run.
$env:TRACY_NO_EXIT = '1'
Push-Location $root      # editor resolves asset-cache/ relative to the working dir
try {
  & $editor --profile $Plan $Frames
} finally {
  Pop-Location
  Remove-Item Env:\TRACY_NO_EXIT -ErrorAction SilentlyContinue
}

if (-not $cap.WaitForExit(60000)) { $cap.Kill(); throw 'tracy-capture did not finish within 60s' }
& (Join-Path $tracy 'tracy-csvexport.exe') $trace | Out-File -Encoding ascii $csv
Write-Host "wrote $trace"
Write-Host "wrote $csv"
