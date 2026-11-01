# Compare sninja against the reference Ninja on the same project: parse cost,
# clean-build wall clock across a -j sweep, incremental rebuild after touching
# one header, and the no-op case.  Reports each run so a regression in the
# scheduler shows up as a slowdown rather than a wrong answer.
param(
  [string]$Project = "$env:TEMP\sninja_bench",
  # Default to the native x86-64 build.  NOT C:\choco\bin\ninja.exe, which reports
  # version 1.13.2 but is a 32-bit Chocolatey shim: a .NET launcher that starts
  # the CLR and then execs the real binary.  It spends ~46 ms before it reads a
  # manifest, which made short commands look 4-6x slower than they are and
  # produced badly wrong comparison numbers until this was caught.
  [string]$Reference = 'C:\Users\fish_\AppData\Local\Programs\Python\Python312\Scripts\ninja.exe',
  [string]$Sninja = "$PSScriptRoot\..\build\sninja.exe",
  [int]$Repeats = 3,
  [int]$Units = 120,
  [int[]]$Jobs = @(1, 4, 8, 12, 16, 24)
)

$ErrorActionPreference = 'Continue'

# Refuse to benchmark against a launcher: the numbers would be meaningless, and
# silently meaningless at that.
function Test-IsLauncher([string]$Path) {
  if (-not (Test-Path $Path)) { return $false }
  $bytes = [IO.File]::ReadAllBytes($Path)
  if ($bytes.Length -lt 1024) { return $false }
  $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
  if ($peOffset -le 0 -or $peOffset -ge $bytes.Length - 256) { return $false }
  if ([Text.Encoding]::ASCII.GetString($bytes, $peOffset, 4) -ne "PE`0`0") { return $false }
  # A managed assembly carries the CLI header in data directory 14.
  $text = [Text.Encoding]::ASCII.GetString($bytes)
  if ($text -match 'BSJB') { return $true }
  return $false
}

if (Test-IsLauncher $Reference) {
  Write-Warning @"
$Reference is a managed launcher (.NET), not ninja itself.
  Its startup cost dominates any short-running command, so the comparison below
  would be measuring the launcher rather than ninja.  Pass -Reference with a
  native ninja, e.g.:
    .\scripts\bench.ps1 -Reference '<path to real ninja.exe>'
"@
}


$envDump = cmd /c '"D:\vs2022\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && set'
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') {
    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
  }
}

if (-not (Test-Path "$Project\CMakeLists.txt")) {
  & "$PSScriptRoot\make_bench_project.ps1" -Project $Project -Units $Units | Out-Null
}

function Configure {
  Remove-Item -Recurse -Force "$Project\build" -ErrorAction SilentlyContinue
  cmake -S $Project -B "$Project\build" -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 |
    Out-Null
}

function CleanOutputs {
  Get-ChildItem "$Project\build" -Recurse -Include *.obj -File |
    Remove-Item -Force -ErrorAction SilentlyContinue
  Remove-Item -Force "$Project\build\bench.lib", "$Project\build\bench_exe.exe",
    "$Project\build\.ninja_log", "$Project\build\.ninja_deps" -ErrorAction SilentlyContinue
}

# Run a builder and return { Seconds, Edges, Exit }.
function Measure-Build {
  param([string]$Exe, [string[]]$Extra = @())
  $out = "$env:TEMP\bench_run.txt"
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $p = Start-Process -FilePath $Exe -ArgumentList (@('-C', "$Project\build") + $Extra) `
    -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$env:TEMP\bench_err.txt"
  $p.WaitForExit(600000) | Out-Null
  $sw.Stop()
  # Ninja 1.13 prints no "edge(s) run" summary, so count the per-edge progress
  # lines that both implementations emit to stdout: "[3/123] Linking ...".
  $text = Get-Content $out -Raw
  $edges = ([regex]::Matches($text, '(?m)^\[\d+/\d+\]')).Count
  return @{ Seconds = $sw.Elapsed.TotalSeconds; Edges = $edges; Exit = $p.ExitCode }
}

function Best-Of {
  param([scriptblock]$Action, [int]$Times)
  $best = [double]::MaxValue
  $last = $null
  for ($i = 0; $i -lt $Times; $i++) {
    $r = & $Action
    if ($r.Edges -le 0 -and $i -eq 0) { $best = [double]::MaxValue; break }
    if ($r.Seconds -lt $best) { $best = $r.Seconds }
    $last = $r
  }
  return @{ Seconds = $best; Edges = $last.Edges; Exit = $last.Exit }
}

function Touch-Header {
  (Get-Item "$Project\src\bench.h").LastWriteTime = Get-Date
}

Configure
$edges_total = (Select-String -Path "$Project\build\build.ninja" -Pattern '^build .*: ' -AllMatches).Count
"project: $Units translation units, $edges_total edges in build.ninja"
"repeats: $Repeats (best of)"
""

function Sweep {
  param([string]$Label, [string]$Exe)
  "=== $Label ==="
  "  clean build:"
  foreach ($j in $Jobs) {
    $arg = if ($j -gt 0) { @("-j", "$j") } else { @() }
    $r = Best-Of { CleanOutputs; Measure-Build $Exe $arg } $Repeats
    if ($r.Edges -le 0) {
      "    -j {0,2}  FAILED (exit {1})" -f $j, $r.Exit
    } else {
      "    -j {0,2}  {1,7:N2}s  ({2} edges)" -f $j, $r.Seconds, $r.Edges
    }
  }
}

Sweep 'sninja (default -j = P cores)' $Sninja
Sweep 'reference ninja' $Reference

"=== incremental: touch one shared header ==="
foreach ($pair in @(@('sninja', $Sninja, 16), @('reference', $Reference, 16))) {
  $label = $pair[0]; $exe = $pair[1]; $j = $pair[2]
  # Build once so there is a warm state, then time the header-triggered rebuild.
  CleanOutputs; Measure-Build $exe @("-j", "$j") | Out-Null
  Touch-Header
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $r = Measure-Build $exe @("-j", "$j")
  $sw.Stop()
  "  {0,-10} -j {1,2}  {2,7:N2}s  ({3} edges)" -f $label, $j, $sw.Elapsed.TotalSeconds, $r.Edges
}

"=== no-op (nothing to do) ==="
foreach ($pair in @(@('sninja', $Sninja, 16), @('reference', $Reference, 16))) {
  $label = $pair[0]; $exe = $pair[1]; $j = $pair[2]
  CleanOutputs; Measure-Build $exe @("-j", "$j") | Out-Null
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $r = Measure-Build $exe @("-j", "$j")
  $sw.Stop()
  "  {0,-10} -j {1,2}  {2,7:N3}s  ({3} edges)" -f $label, $j, $sw.Elapsed.TotalSeconds, $r.Edges
}

"=== parse cost (-t targets on a large manifest) ==="
foreach ($pair in @(@('sninja', $Sninja), @('reference', $Reference))) {
  $label = $pair[0]; $exe = $pair[1]
  $best = [double]::MaxValue
  for ($i = 0; $i -lt $Repeats; $i++) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $Exe -ArgumentList @('-C', "$Project\build", '-t', 'targets') `
      -NoNewWindow -PassThru -RedirectStandardOutput "$env:TEMP\pt.txt" -RedirectStandardError "$env:TEMP\pt_err.txt"
    $p.WaitForExit(60000) | Out-Null
    $sw.Stop()
    if ($sw.Elapsed.TotalSeconds -lt $best) { $best = $sw.Elapsed.TotalSeconds }
  }
  "  {0,-10} {1,7:N3}s" -f $label, $best
}
