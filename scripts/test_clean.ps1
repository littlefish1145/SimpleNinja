# Verify the file-removal tools: -t clean (in its forms) and -t clean dead.
#
# The part worth automating is the safety property: these tools delete files, so
# the interesting question is never "does it delete", it is "does it ever delete
# something the build still needs".
param(
  [string]$Project = "$env:TEMP\sninja_proj",
  [string]$RestatDir = "$env:TEMP\sninja_restat_test",
  [string]$Sninja = "$PSScriptRoot\..\build\sninja.exe"
)

$BuildDir = Join-Path $Project "build"
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
  param([string]$Dir, [string[]]$Extra = @())
  $out = "$env:TEMP\clean_out.txt"
  $p = Start-Process -FilePath $Sninja -ArgumentList (@('-C', $Dir) + $Extra) `
    -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$env:TEMP\clean_err.txt"
  $p.WaitForExit(60000) | Out-Null
  return @{ Exit = $p.ExitCode; Out = [string](Get-Content $out -Raw) }
}

"--- clean dead ---"
Run-Sninja $BuildDir @('-t', 'clean') | Out-Null
Remove-Item -Force "$BuildDir\.ninja_log" -ErrorAction SilentlyContinue
Run-Sninja $BuildDir @('-j', '8') | Out-Null

# An output the manifest no longer mentions, as a previous configuration left.
Set-Content "$BuildDir\orphaned.obj" -Value 'junk' -NoNewline
Add-Content "$BuildDir\.ninja_log" -Value "0`t0`t0`torphaned.obj`tdeadbeef"

$r = Run-Sninja $BuildDir @('-t', 'clean', 'dead', '-n')
Check 'dry run reports the orphan' ($r.Out -match 'orphaned\.obj') $r.Out.Trim()
Check 'dry run deletes nothing' (Test-Path "$BuildDir\orphaned.obj")

$r = Run-Sninja $BuildDir @('-t', 'clean', 'dead')
Check 'orphan removed' (-not (Test-Path "$BuildDir\orphaned.obj"))
Check 'live outputs survive clean dead' ((Test-Path "$BuildDir\core.lib") -and
      (Test-Path "$BuildDir\app.exe"))

"--- clean dead must spare files the graph still knows ---"
Run-Sninja $RestatDir | Out-Null
# in.txt is a source: in the graph as an input, with no edge producing it.
Add-Content "$RestatDir\.ninja_log" -Value "0`t0`t0`tin.txt`tdeadbeef"
Set-Content "$RestatDir\stray.tmp" -Value 'x' -NoNewline
Add-Content "$RestatDir\.ninja_log" -Value "0`t0`t0`tstray.tmp`tdeadbeef"

$r = Run-Sninja $RestatDir @('-t', 'clean', 'dead', '-n')
Check 'source file in the graph is spared' (-not ($r.Out -match 'in\.txt')) $r.Out.Trim()
Check 'unreferenced path is reported' ($r.Out -match 'stray\.tmp') $r.Out.Trim()

$r = Run-Sninja $RestatDir @('-t', 'clean', 'dead')
Check 'source file untouched on disk' (Test-Path "$RestatDir\in.txt")
Check 'unreferenced path removed' (-not (Test-Path "$RestatDir\stray.tmp"))

"--- clean, in its forms ---"
Run-Sninja $BuildDir @('-t', 'clean') | Out-Null
# Check before rebuilding: the rebuild puts the outputs back.
Check 'clean removed the outputs' (-not (Test-Path "$BuildDir\core.lib"))
$r = Run-Sninja $BuildDir @('-j', '8')
Check 'build after clean works' ($r.Exit -eq 0 -and $r.Out -match '(\d+) edge\(s\) run') $r.Out.Trim()


Run-Sninja $BuildDir @('-t', 'clean', 'core') | Out-Null
Check 'clean <target> removes that target' (-not (Test-Path "$BuildDir\core.lib"))
Check 'clean <target> leaves others' (Test-Path "$BuildDir\app.exe")

$r = Run-Sninja $BuildDir @('-t', 'clean', 'rules', 'CXX_EXECUTABLE_LINKER__app_Release')
Check 'clean rules removes that rule output' (-not (Test-Path "$BuildDir\app.exe"))
$r = Run-Sninja $BuildDir @('-t', 'clean', 'rules', 'NoSuchRule')
Check 'clean rules with an unknown rule fails' ($r.Exit -ne 0) "exit $($r.Exit)"

if ($failures -eq 0) { "`nALL CLEAN CHECKS PASSED" } else { "`n$failures CHECK(S) FAILED" }
exit $failures
