# Headless streaming-benchmark sweep for VoxelWorlds.
#
# Runs each velocity as a FRESH "UnrealEditor-Cmd -game -nullrhi -VoxelForceCPU" process, so every
# run is COLD (no chunks lingering from a prior run) and FOCUS-IMMUNE (no editor window => no
# "Use Less CPU when in Background" throttle distorting frame-time / generation throughput).
# Collects the JSON reports written to Saved/VoxelBench/ and prints a comparison table.
#
# Usage:
#   powershell -File Plugins\VoxelWorlds\Scripts\voxel_bench_sweep.ps1 -Velocities 1500,3000,6000 -Seconds 10
#   # cubic vs the map's own config, same map:
#   powershell -File ...\voxel_bench_sweep.ps1 -Prefix cubic -Config /Game/PluginTesting/Review/Review_Cubic
#   # island chord with terrain following (bench args use ':' and '/', never commas):
#   powershell -File ...\voxel_bench_sweep.ps1 -Prefix islandM -Config /Game/PluginTesting/Review/Review_Island_M `
#     -Distance 500000 -BenchArgs "start=-250000:0 dir=1:0 follow=300"
#   # cvar A/B (run in-process with & so -Velocities binds as int[]):
#   & ...\voxel_bench_sweep.ps1 -Prefix nofarfield -PreCmds "voxel.World.FarField 0"
#
# Paths self-locate from the script location; override -EditorCmd if your engine lives elsewhere.
param(
  [int[]]$Velocities = @(1500, 3000, 6000),
  [double]$Seconds   = 10.0,                 # traverse seconds; distance = velocity * seconds
  [double]$Distance  = 0,                    # > 0: fixed traverse distance for every velocity (overrides -Seconds)
  [string]$Prefix    = "sweep",
  [string]$Map       = "/Game/PluginTesting/VoxelWorldsTest",
  # Object path of a UVoxelWorldConfiguration to run instead of the map's own (-VoxelConfig=).
  [string]$Config    = "",
  # Extra voxel.Bench.Run key=value arguments (dir=, start=, route=, orbit=, follow=, warmup=, catchup=).
  [string]$BenchArgs = "",
  [int]$TimeoutSec   = 220,
  [string]$EditorCmd = "D:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe",
  # Extra command-line args appended to every run, e.g. scheduler A/B overrides:
  #   -ExtraArgs "-VoxelMaxAsyncMesh=16 -VoxelMaxAsyncGen=8 -VoxelPinScheduler"
  [string]$ExtraArgs = "",
  # Console commands run before the bench starts, e.g. a cvar A/B:
  #   -PreCmds "voxel.World.FarField 0"
  [string[]]$PreCmds = @()
)

if ($BenchArgs -match ",") { Write-Error "-BenchArgs must not contain commas: -ExecCmds splits commands on them. Use ':' and '/'."; exit 1 }
if (($PreCmds -join "") -match ",") { Write-Error "-PreCmds entries must not contain commas: -ExecCmds splits commands on them."; exit 1 }
if (-not (Test-Path $EditorCmd)) { Write-Error "UnrealEditor-Cmd not found at $EditorCmd (pass -EditorCmd)"; exit 1 }

# .../Plugins/VoxelWorlds/Scripts -> project root is three levels up.
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
$uproject    = (Get-ChildItem (Join-Path $projectRoot "*.uproject") | Select-Object -First 1).FullName
$benchDir    = Join-Path $projectRoot "Saved\VoxelBench"
if (-not $uproject) { Write-Error "No .uproject found under $projectRoot"; exit 1 }

# Record which VoxelWorlds commit produced the numbers (the report has no other way to know).
$build = (& git -C (Join-Path $PSScriptRoot "..") rev-parse --short HEAD 2>$null)
if (-not $build) { $build = "unknown" }
$dirty = (& git -C (Join-Path $PSScriptRoot "..") status --porcelain --untracked-files=no 2>$null)
if ($dirty) { $build = "$build-dirty" }

$configArg = if ($Config) { "-VoxelConfig=$Config" } else { "" }

$results = @()
foreach ($v in $Velocities) {
  $dist = if ($Distance -gt 0) { [int]$Distance } else { [int]($v * $Seconds) }
  $tag  = "${Prefix}_v$v"
  $log  = Join-Path $projectRoot "Saved\sweep_$tag.log"
  $before = @(Get-ChildItem "$benchDir\*_$tag.json" -ErrorAction SilentlyContinue | Select-Object -Expand FullName)
  $benchCmd = (@($PreCmds) + @("voxel.Bench.Run $tag $v $dist build=$build $BenchArgs".Trim())) -join ","
  $cmdArgs = "`"$uproject`" $Map -game -nullrhi -VoxelForceCPU -unattended -nosplash $configArg $ExtraArgs -ExecCmds=`"$benchCmd`" -abslog=`"$log`""
  Write-Host "=== $tag (velocity=$v dist=$dist build=$build $configArg) ==="
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
      vel=$v; mode="$($j.run.worldMode)/$($j.run.meshingMode)"; frameMsP95=$j.frameMsP95; totalMsP95=$j.totalMsP95;
      catchUpSec=$j.catchUpSec; peakMeshQ=$j.peakMeshQueue; peakGenQ=$j.peakGenQueue; peakLoaded=$j.peakLoadedChunks;
      peakVoxelMB=$j.memory.peakVoxelSystemsMB; thrash=$j.thrashRemeshCount; audit=$j.auditFindings
    }
  } else {
    $results += [pscustomobject]@{ vel=$v; mode=""; frameMsP95="NO REPORT"; totalMsP95=""; catchUpSec=""; peakMeshQ=""; peakGenQ=""; peakLoaded=""; peakVoxelMB=""; thrash=""; audit="" }
  }
}

Write-Host "`n===== SWEEP SUMMARY (headless CPU, focus-immune, build $build) ====="
$results | Format-Table -AutoSize
