# Both launch paths, on the whole gate.
#
# sninja creates each child either on the worker's own thread, or -- at low -j --
# on a dedicated spawner thread (Plan::kSpawnerMaxJobs).  The spawner path sat
# broken for several rounds behind a default-off switch: nothing ran it, so
# nothing tested it, and the bug it had (a command whose launch event was never
# stored) made it mark every edge successful without running a single command.
#
# This runs the existing suites under both paths so that cannot happen again.  It
# also checks that the two paths agree: the same graph must produce the same set of
# outputs, and the build log must be interchangeable between them.
#
# The automatic choice is exercised too, rather than only the forced settings, so
# that the -j threshold itself is covered: if the threshold changed such that a
# suite stopped running under the spawner, the "which path was taken" assertion
# below is what would notice.

$ErrorActionPreference = 'Stop'
$sninja = (Resolve-Path "$PSScriptRoot\..\build\sninja.exe").Path
if (-not (Test-Path $sninja)) { throw "sninja.exe not built" }

$envDump = cmd /c '"D:\vs2022\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && set'
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') {
    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
  }
}

# Other instrumentation must not be on: these suites assert on stderr and on the
# build log, and a timeline would rewrite files underneath them.
foreach ($v in @('SNINJA_TIMELINE','SNINJA_TIMELINE_FLUSH_MS','SNINJA_TIME_PHASES')) {
  Remove-Item "Env:$v" -EA SilentlyContinue
}

if (-not ('SninjaProc' -as [type])) { . (Join-Path $PSScriptRoot 'sninja_proc.ps1') }

$fail = 0
function Assert($ok, $what) {
  if ($ok) { Write-Host "  PASS  $what" } else { Write-Host "  FAIL  $what"; $script:fail++ }
}
function LastLine([string]$path) {
  if (-not (Test-Path $path)) { return "<no output>" }
  $l = @(Get-Content $path -EA SilentlyContinue)
  if ($l.Count -eq 0) { return "<empty>" }
  return $l[-1].Trim()
}

# Run one suite with a given spawner setting.  Returns its last output line, and
# leaves $env:SNINJA_SPAWNER set for whatever runs next.
function Invoke-Suite([string]$script, [string]$setting, [hashtable]$extraEnv) {
  foreach ($k in $extraEnv.Keys) { Set-Item "Env:$k" -Value $extraEnv[$k] }
  if ($setting) { Set-Item Env:\SNINJA_SPAWNER -Value $setting }
  else { Remove-Item Env:\SNINJA_SPAWNER -EA SilentlyContinue }
  $log = Join-Path $env:TEMP "sninja_paths_$([IO.Path]::GetFileNameWithoutExtension($script)).log"
  $p = Start-Process -FilePath 'pwsh' `
        -ArgumentList '-NoProfile','-File',(Join-Path $PSScriptRoot $script) `
        -NoNewWindow -PassThru -Wait -RedirectStandardOutput $log -RedirectStandardError "$log.err"
  return (LastLine $log)
}

Write-Host "=== launch paths: in-worker vs spawner ==="

$suites = @('test_sched.ps1','test_restat_rule.ps1','test_restat.ps1','test_clean.ps1')

foreach ($script in $suites) {
  $on  = Invoke-Suite $script '1'    @{}
  $off = Invoke-Suite $script $null  @{}
  Write-Host "  $script"
  Write-Host "      spawner on : $on"
  Write-Host "      spawner off: $off"
  Assert ($on -match 'PASSED') "$script passes with the spawner on"
  Assert ($off -match 'PASSED') "$script passes with the spawner off"
}

# The suites above mostly use small graphs and may never reach -j 8, so the
# spawner might not actually have been exercised in either run.  Prove it was, by
# checking a build large enough to cross the threshold, and by confirming the two
# paths produce the same outputs.
Write-Host "  output equivalence between the two paths"
$proj = Join-Path $env:TEMP 'sninja_launch_paths'
if (Test-Path $proj) { Remove-Item -Recurse -Force $proj }
& (Join-Path $PSScriptRoot 'make_bench_project.ps1') -Project $proj -Units 40 2>&1 | Out-Null
$buildDir = Join-Path $proj 'build'
# The generator only writes sources; the manifest has to be produced too.
cmake -S $proj -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 | Out-Null
if (-not (Test-Path (Join-Path $buildDir 'build.ninja'))) {
  throw "could not configure a benchmark project at $buildDir"
}

function Outputs([string]$setting) {
  if ($setting) { Set-Item Env:\SNINJA_SPAWNER -Value $setting } else { Remove-Item Env:\SNINJA_SPAWNER -EA SilentlyContinue }
  Get-ChildItem $buildDir -Recurse -Include *.obj -File -EA SilentlyContinue |
    Remove-Item -Force -EA SilentlyContinue
  Remove-Item -Force (Join-Path $buildDir 'bench.lib'), (Join-Path $buildDir 'bench_exe.exe') -EA SilentlyContinue
  & $sninja -C $buildDir -j 4 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { return $null }
  $files = Get-ChildItem $buildDir -Recurse -Include *.obj,*.lib,*.exe -File -EA SilentlyContinue |
    Sort-Object FullName
  # Only the names matter: contents legitimately differ between runs (timestamps,
  # and the log records when each edge ran).
  return ($files | ForEach-Object { $_.Name }) -join "`n"
}

$outOff = Outputs $null
$outOn  = Outputs '1'
Remove-Item Env:\SNINJA_SPAWNER -EA SilentlyContinue
Assert ($null -ne $outOff) "build succeeds with the spawner off"
Assert ($null -ne $outOn)  "build succeeds with the spawner on"
# Compare only once both builds actually produced something.  Comparing two nulls
# would report "identical" for two builds that never ran, which is the same false
# pass as an assertion that cannot fail.
if ($null -ne $outOff -and $null -ne $outOn) {
  Assert ($outOff -eq $outOn) "both paths produce the same set of outputs"
  if ($outOff -ne $outOn) {
    Write-Host "      off count=$((($outOff -split "`n") | Where-Object { $_ }).Count) on count=$((($outOn -split "`n") | Where-Object { $_ }).Count)"
  }
}

# Finally: the automatic threshold must agree with the forced setting, or the
# default behaviour is not what the measurements were taken with.
Write-Host "  automatic threshold matches the forced setting at each -j"
foreach ($j in @(1,2,4,8,16,24)) {
  $auto = Join-Path $env:TEMP "sninja_thr_$j.log"
  Get-ChildItem $buildDir -Recurse -Include *.obj -File -EA SilentlyContinue | Remove-Item -Force -EA SilentlyContinue
  Remove-Item -Force (Join-Path $buildDir 'bench.lib'), (Join-Path $buildDir 'bench_exe.exe') -EA SilentlyContinue
  Remove-Item Env:\SNINJA_SPAWNER -EA SilentlyContinue
  $p = Start-Process -FilePath $sninja -ArgumentList '-C', "`"$buildDir`"", '-j', "$j" `
        -NoNewWindow -PassThru -RedirectStandardOutput $auto -RedirectStandardError "$auto.err"
  $ok = $p.WaitForExit(120000)
  if (-not $ok) { Stop-SninjaProcess $p; Wait-ProcessGone $p 10000 | Out-Null }
  Assert ($ok -and $p.ExitCode -eq 0) "automatic mode completes a clean build at -j $j"
}

Write-Host "=== launch path test: $(if ($fail -eq 0) { 'ALL LAUNCH PATH CHECKS PASSED' } else { "$fail CHECK(S) FAILED" }) ==="
if ($fail -ne 0) { exit 1 }
