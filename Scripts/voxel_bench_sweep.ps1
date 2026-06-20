# Headless streaming-benchmark sweep for VoxelWorlds.
#
# Runs each velocity as a FRESH "UnrealEditor-Cmd -game -nullrhi -VoxelForceCPU" process, so every
# run is COLD (no chunks lingering from a prior run) and FOCUS-IMMUNE (no editor window => no
# "Use Less CPU when in Background" throttle distorting frame-time / generation throughput).
# Collects the JSON reports written to Saved/VoxelBench/ and prints a comparison table.
#
# Usage:
#   powershell -File Plugins\VoxelWorlds\Scripts\voxel_bench_sweep.ps1 -Velocities 1500,3000,6000 -Seconds 10
#
# Paths self-locate from the script location; override -EditorCmd if your engine lives elsewhere.
param(
  [int[]]$Velocities = @(1500, 3000, 6000),
  [double]$Seconds   = 10.0,                 # traverse seconds; distance = velocity * seconds
  [string]$Prefix    = "sweep",
  [string]$Map       = "/Game/PluginTesting/VoxelWorldsTest",
  [int]$TimeoutSec   = 220,
  [string]$EditorCmd = "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
)

# .../Plugins/VoxelWorlds/Scripts -> project root is three levels up.
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$uproject    = (Get-ChildItem (Join-Path $projectRoot "*.uproject") | Select-Object -First 1).FullName
$benchDir    = Join-Path $projectRoot "Saved\VoxelBench"
if (-not $uproject) { Write-Error "No .uproject found under $projectRoot"; exit 1 }

$results = @()
foreach ($v in $Velocities) {
  $dist = [int]($v * $Seconds)
  $tag  = "${Prefix}_v$v"
  $log  = Join-Path $projectRoot "Saved\sweep_$tag.log"
  $before = @(Get-ChildItem "$benchDir\*_$tag.json" -ErrorAction SilentlyContinue | Select-Object -Expand FullName)
  $cmdArgs = "`"$uproject`" $Map -game -nullrhi -VoxelForceCPU -unattended -nosplash -ExecCmds=`"voxel.Bench.Run $tag $v $dist`" -abslog=`"$log`""
  Write-Host "=== $tag (velocity=$v dist=$dist) ==="
  $p = Start-Process $EditorCmd -ArgumentList $cmdArgs -PassThru
  $report = $null; $deadline = (Get-Date).AddSeconds($TimeoutSec)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 6
    $f = Get-ChildItem "$benchDir\*_$tag.json" -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.FullName } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($f) { $report = $f.FullName; break }
    if ($p.HasExited) { break }
  }
  if (-not $p.HasExited) { Stop-Process $p -Force }
  if ($report) {
    $j = Get-Content $report -Raw | ConvertFrom-Json
    $results += [pscustomobject]@{
      vel=$v; frameMsP95=$j.frameMsP95; catchUpSec=$j.catchUpSec; peakMeshQ=$j.peakMeshQueue;
      peakGenQ=$j.peakGenQueue; peakLoaded=$j.peakLoadedChunks; thrash=$j.thrashRemeshCount; unloadDistMaxUU=$j.unloadDistMaxUU
    }
  } else {
    $results += [pscustomobject]@{ vel=$v; frameMsP95="NO REPORT"; catchUpSec=""; peakMeshQ=""; peakGenQ=""; peakLoaded=""; thrash=""; unloadDistMaxUU="" }
  }
}

Write-Host "`n===== SWEEP SUMMARY (headless CPU, focus-immune) ====="
$results | Format-Table -AutoSize
