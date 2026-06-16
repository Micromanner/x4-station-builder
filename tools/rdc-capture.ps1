#requires -Version 5
<#
.SYNOPSIS
  Headless RenderDoc frame capture of the X4 station builder.
.DESCRIPTION
  Launches the editor in --rdc mode UNDER RenderDoc (renderdoccmd capture injects
  renderdoc.dll). The editor's in-app API then brackets exactly one frame and a .rdc
  is written. Open it in tools/renderdoc/qrenderdoc.exe to inspect the draw-call
  list, bound textures/buffers, shaders, and pipeline state for that frame.
.EXAMPLE
  tools/rdc-capture.ps1 -Plan "C:\...\Sphere Shipyard.xml" -Name sphere
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory)] [string]$Plan,
  [string]$Name = 'capture'
)
$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $PSScriptRoot
$rdcmd  = Join-Path $PSScriptRoot 'renderdoc/renderdoccmd.exe'
$editor = Join-Path $root 'build/full/bin/x4sb-editor.exe'
$outDir = Join-Path $root 'profiling'
$prefix = Join-Path $outDir $Name

if (-not (Test-Path $rdcmd))  { throw "RenderDoc CLI missing: $rdcmd  (see tools/renderdoc/README.md to fetch it)" }
if (-not (Test-Path $editor)) { throw "editor not built: $editor  (run: cmake --build --preset full)" }
if (-not (Test-Path $Plan))   { throw "plan not found: $Plan" }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# capture: launch the editor with renderdoc.dll injected.
#   -w  wait until the editor exits so the capture is flushed
#   -d  working dir = repo root, so the editor finds asset-cache/
#   -c  .rdc filename template (RenderDoc appends _frameNNN)
# Args after the executable (--rdc <plan>) are passed straight through to the editor.
& $rdcmd capture -w -d $root -c $prefix $editor --rdc $Plan

$rdc = Get-ChildItem $outDir -Filter "$Name*.rdc" -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime | Select-Object -Last 1
if ($rdc) {
  Write-Host "wrote $($rdc.FullName)"
  Write-Host "open with: tools/renderdoc/qrenderdoc.exe `"$($rdc.FullName)`""
} else {
  Write-Warning 'no .rdc produced — check that the editor reported under_renderdoc=yes'
}
