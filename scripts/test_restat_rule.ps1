# Verify -t restat against the case it actually exists for: an edge declared
# `restat = 1`.  For a restat edge the output's own mtime proves nothing (the
# command is allowed not to touch it), so the dirty decision falls back to the
# mtime recorded in the build log.  If that recorded value is stale -- which is
# what an interrupted or failed command leaves behind -- the edge is rebuilt on
# every single run, and `-t restat` is what repairs it.
param(
  [string]$Sninja = "$PSScriptRoot\..\build\sninja.exe"
)

$envDump = cmd /c '"D:\vs2022\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && set'
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') {
    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
  }
}

$dir = "$env:TEMP\sninja_restat_test"
Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $dir | Out-Null
Set-Content "$dir\in.txt" -Value "hello" -NoNewline

# Ninja requires the final line to be newline-terminated.
@'
rule stage
  command = cmd /c copy /Y in.txt out.txt
  restat = 1

build out.txt: stage in.txt
build all: phony out.txt
'@ | Set-Content "$dir\build.ninja"

$failures = 0
function Check {
  param([string]$name, [bool]$ok, [string]$detail = '')
  if ($ok) { "  PASS  $name" } else { $script:failures++; "  FAIL  $name  $detail" }
}
function Run-Sninja {
  param([string[]]$Extra = @())
  $out = "$env:TEMP\restat2_out.txt"
  $p = Start-Process -FilePath $Sninja -ArgumentList (@('-C', $dir) + $Extra) `
    -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$env:TEMP\restat2_err.txt"
  $p.WaitForExit(60000) | Out-Null
  $text = Get-Content $out -Raw
  $edges = if ($text -match '(\d+) edge\(s\) run') { [int]$matches[1] } else { -1 }
  return @{ Edges = $edges; Exit = $p.ExitCode }
}
function Log-Mtime {
  foreach ($line in Get-Content "$dir\.ninja_log" -ErrorAction SilentlyContinue) {
    if ($line -like "#*") { continue }
    $f = $line -split "`t"
    if ($f.Count -ge 5 -and $f[3] -eq 'out.txt') { return [int64]$f[2] }
  }
  return -1
}

$r = Run-Sninja
Check 'restat edge builds' ($r.Edges -ge 1) "got $($r.Edges) edges"
Check 'output produced' (Test-Path "$dir\out.txt")
Check 'mtime recorded' ((Log-Mtime) -gt 0) "got $(Log-Mtime)"

$r = Run-Sninja
Check 'no-op after first build' ($r.Edges -eq 0) "got $($r.Edges) edges"

# Now the failure mode restat exists to repair: the log claims an mtime older
# than the input, so the restat edge looks dirty forever.
$lines = Get-Content "$dir\.ninja_log"
$patched = foreach ($line in $lines) {
  if ($line -like "#*") { $line; continue }
  $f = $line -split "`t"
  "$($f[0])`t$($f[1])`t1`t$($f[3])`t$($f[4])"
}
Set-Content "$dir\.ninja_log" -Value $patched

$r = Run-Sninja
Check 'stale restat log forces a rebuild' ($r.Edges -ge 1) "got $($r.Edges) edges"

# Re-stale it, then repair with -t restat.
$lines = Get-Content "$dir\.ninja_log"
$patched = foreach ($line in $lines) {
  if ($line -like "#*") { $line; continue }
  $f = $line -split "`t"
  "$($f[0])`t$($f[1])`t1`t$($f[3])`t$($f[4])"
}
Set-Content "$dir\.ninja_log" -Value $patched

$r = Run-Sninja @('-t', 'restat')
Check 'restat exits 0' ($r.Exit -eq 0) "exit $($r.Exit)"
Check 'restat refreshed the recorded mtime' ((Log-Mtime) -gt 1) "got $(Log-Mtime)"

$r = Run-Sninja
Check 'no-op after restat' ($r.Edges -eq 0) "got $($r.Edges) edges"
$r = Run-Sninja
Check 'still a no-op on the next run' ($r.Edges -eq 0) "got $($r.Edges) edges"

if ($failures -eq 0) { "`nALL RESTAT CHECKS PASSED" } else { "`n$failures CHECK(S) FAILED" }
exit $failures
