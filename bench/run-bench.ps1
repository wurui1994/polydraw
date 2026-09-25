# run-bench.ps1 -- run every .pss through the instrumented polydraw and collect
# one "script<TAB>fps<TAB>ms_per_frame" row per script into a TSV.
#
# Usage (from this directory, on the Windows box):
#     powershell -ExecutionPolicy Bypass -File run-bench.ps1 -Frames 300
#
# Two traps this script exists to avoid -- both were measured, not guessed:
#
#  1. File names with spaces.  `Start-Process -ArgumentList "a b.pss"` splits
#     them into two arguments and polydraw silently draws nothing; 17 scripts
#     (including "snake tube", "disco ball", "town textured" -- exactly the ones
#     that matter) vanished from the first run this way.  Every path is quoted.
#  2. Window size.  polydraw's render window follows the saved layout, so two
#     runs on two machines are not comparable unless the size is pinned.  Pass
#     -Width/-Height and the script writes them into polydraw.ini before each
#     run (that is where the stock program keeps them).
param(
  [int]$Frames = 300,
  [int]$Width  = 320,
  [int]$Height = 240,
  [string]$Exe = "..\polydraw_src\polydraw.exe",
  [string]$Out = "pdref-fps.tsv"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root
$exe = (Resolve-Path $Exe).Path
$log = Join-Path (Split-Path -Parent $exe) 'polydraw_bench.txt'

# A fresh log per run: the instrumentation appends, so a stale file would mix
# two runs' rows and the newest row is not necessarily last.
if (Test-Path $log) { Remove-Item $log }

$scripts = Get-ChildItem -Path (Join-Path $root '..') -Recurse -Filter *.pss |
           Sort-Object FullName
Write-Host "$($scripts.Count) scripts, $Frames timed frames each (30 warmup)"

foreach ($s in $scripts) {
  # Quote both the script and the switch: see trap 1 above.
  $args = @("`"$($s.FullName)`"", "/bench:$Frames")
  $p = Start-Process -FilePath $exe -ArgumentList $args -PassThru -Wait `
                     -WorkingDirectory (Split-Path -Parent $exe)
  if ($p.ExitCode -ne 0) { Write-Host "  (exit $($p.ExitCode)) $($s.Name)" }
}

if (-not (Test-Path $log)) { throw "no $log -- did any run finish?" }
Copy-Item $log (Join-Path $root $Out) -Force
Write-Host "wrote $Out ($((Get-Content (Join-Path $root $Out)).Count) rows)"
