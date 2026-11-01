# Verify -t restat does what it exists for: recover a build log whose recorded
# mtimes are older than the files they describe, which is what an interrupted or
# failed command leaves behind and what makes every later run rebuild forever.
param(
  [string]$Project = "$env:TEMP\sninja_proj",
  [string]$Reference = 'C:\choco\bin\ninja.exe',
  [string]$Sninja = "$PSScriptRoot\..\build\sninja.exe"
)

$envDump = cmd /c '"D:\vs2022\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && set'
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') {
    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
  }
}

$failures = 0
function Check {
  param([string]$name, [bool]$ok, [string]$detail = '')
  if ($ok) { "  PASS  $name" } else { $script:failures++; "  FAIL  $name  $detail" }
}
function Run-Sninja {
  param([string[]]$Extra = @())
  $out = "$env:TEMP\restat_out.txt"
  $p = Start-Process -FilePath $Sninja -ArgumentList (@('-C', "$Project\build") + $Extra) `
    -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$env:TEMP\restat_err.txt"
  $p.WaitForExit(120000) | Out-Null
  $text = Get-Content $out -Raw
  $edges = if ($text -match '(\d+) edge\(s\) run') { [int]$matches[1] } else { -1 }
  return @{ Edges = $edges; Exit = $p.ExitCode }
}
function Log-Mtime {
  param([string]$Output)
  foreach ($line in Get-Content "$Project\build\.ninja_log") {
    if ($line -like "#*") { continue }
    $f = $line -split "`t"
    if ($f.Count -ge 5 -and $f[3] -eq $Output) { return [int64]$f[2] }
  }
  return -1
}

# Start from a clean, fully built state written by sninja itself.
Run-Sninja @('-t', 'clean') | Out-Null
Remove-Item -Force "$Project\build\.ninja_log" -ErrorAction SilentlyContinue
$r = Run-Sninja @('-j', '8')
Check 'clean build' ($r.Edges -ge 5) "got $($r.Edges) edges"

$mtime_before = Log-Mtime 'app.exe'
Check 'app.exe has a recorded mtime' ($mtime_before -gt 0) "got $mtime_before"

# Simulate an interrupted command: the log claims an mtime far older than the
# file really has.
$lines = Get-Content "$Project\build\.ninja_log"
$patched = foreach ($line in $lines) {
  if ($line -like "#*") { $line; continue }
  $f = $line -split "`t"
  if ($f.Count -ge 5 -and $f[3] -eq 'app.exe') {
    "$($f[0])`t$($f[1])`t1`t$($f[3])`t$($f[4])"
  } else { $line }
}
Set-Content "$Project\build\.ninja_log" -Value $patched
Check 'log now records a stale mtime' ((Log-Mtime 'app.exe') -eq 1) "got $(Log-Mtime 'app.exe')"

# A stale recorded mtime does NOT by itself make an ordinary (non-restat) edge
# dirty: those are judged against the output's real mtime, which is still newer
# than the inputs.  This matches upstream ninja.  Only `restat = 1` edges consult
# the recorded value -- that case is covered by test_restat_rule.ps1.
$r = Run-Sninja @('-j', '8')
Check 'stale log alone does not dirty a normal edge' ($r.Edges -eq 0) "got $($r.Edges) edges"

$r = Run-Sninja @('-t', 'restat')
Check 'restat exits 0' ($r.Exit -eq 0) "exit $($r.Exit)"
$after = Log-Mtime 'app.exe'
Check 'restat updated the recorded mtime' ($after -gt 1) "got $after"

$r = Run-Sninja @('-j', '8')
Check 'build is a no-op after restat' ($r.Edges -eq 0) "got $($r.Edges) edges"

# The rewritten log must still be readable by the reference implementation.
$refOut = "$env:TEMP\restat_ref.txt"
$p = Start-Process -FilePath $Reference -ArgumentList @('-C', "$Project\build", '-n') `
  -NoNewWindow -PassThru -RedirectStandardOutput $refOut -RedirectStandardError "$env:TEMP\restat_ref_err.txt"
$p.WaitForExit(60000) | Out-Null
$refText = (Get-Content $refOut -Raw) + (Get-Content "$env:TEMP\restat_ref_err.txt" -Raw)
Check 'reference ninja sees no work' ($refText -match 'no work to do') $refText.Trim()

# Restat of a single named output must leave the others alone.
$lines = Get-Content "$Project\build\.ninja_log"
$patched = foreach ($line in $lines) {
  if ($line -like "#*") { $line; continue }
  $f = $line -split "`t"
  if ($f.Count -ge 5 -and $f[3] -eq 'core.lib') { "$($f[0])`t$($f[1])`t1`t$($f[3])`t$($f[4])" } else { $line }
}
Set-Content "$Project\build\.ninja_log" -Value $patched
$r = Run-Sninja @('-t', 'restat', 'core.lib')
Check 'restat of one output exits 0' ($r.Exit -eq 0) "exit $($r.Exit)"
Check 'named output was restatted' ((Log-Mtime 'core.lib') -gt 1) "got $(Log-Mtime 'core.lib')"
$r = Run-Sninja @('-j', '8')
Check 'build is a no-op after targeted restat' ($r.Edges -eq 0) "got $($r.Edges) edges"

if ($failures -eq 0) { "`nALL RESTAT CHECKS PASSED" } else { "`n$failures CHECK(S) FAILED" }
exit $failures
