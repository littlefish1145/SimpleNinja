# Exercise sninja on the sample project: clean build, no-op, targeted rebuild,
# and a sweep over -j values, each with a wall-clock timeout so a scheduler
# deadlock shows up as a failure instead of a hang.
param(
  [string]$Project = "$env:TEMP\sninja_proj",
  [string]$Sninja = "$PSScriptRoot\..\build\sninja.exe",
  [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = 'Continue'

# CMake needs the MSVC environment (LIB/INCLUDE/PATH) to configure and to
# compile, and sninja sets up its own copy for its children.
$envDump = cmd /c '"D:\vs2022\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && set'
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') {
    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
  }
}

function Configure {
  Remove-Item -Recurse -Force "$Project\build" -ErrorAction SilentlyContinue
  cmake -S $Project -B "$Project\build" -G Ninja -DCMAKE_BUILD_TYPE=Release 2>&1 |
    Out-Null
}

# Wipe every build output but keep the manifest and rules intact.
function CleanOutputs {
  Get-ChildItem "$Project\build" -Recurse -Include *.obj, *.o -File |
    Remove-Item -Force -ErrorAction SilentlyContinue
  Remove-Item -Force "$Project\build\core.lib", "$Project\build\app.exe",
    "$Project\build\.ninja_log", "$Project\build\.ninja_deps" -ErrorAction SilentlyContinue
}

function Invoke-Sninja {
  param([string[]]$Extra = @())
  $out = "$env:TEMP\sninja_run.txt"
  $p = Start-Process -FilePath $Sninja -ArgumentList (@('-C', "$Project\build") + $Extra) `
    -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError "$env:TEMP\sninja_run_err.txt"
  if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
    $p.Kill()
    return @{ TimedOut = $true; Exit = -1; Out = '' }
  }
  return @{ TimedOut = $false; Exit = $p.ExitCode; Out = (Get-Content $out -Raw) }
}

function EdgeCount([string]$text) {
  if ($text -match '(\d+) edge\(s\) run') { return [int]$matches[1] }
  return -1
}

$failures = 0
function Check {
  param([string]$name, [bool]$ok, [string]$detail = '')
  if ($ok) { "  PASS  $name" } else { $script:failures++; "  FAIL  $name  $detail" }
}

Configure

foreach ($j in @('', '-j 1', '-j 2', '-j 4', '-j 8', '-j 24')) {
  $label = if ($j) { $j } else { 'default' }
  "--- $label ---"

  CleanOutputs
  $r = Invoke-Sninja @($j | Where-Object { $_ })
  Check "$label clean build" (-not $r.TimedOut -and $r.Exit -eq 0) "exit=$($r.Exit) $(EdgeCount $r.Out) edges"
  Check "$label clean ran edges" ((EdgeCount $r.Out) -ge 5) "got $(EdgeCount $r.Out)"

  $r = Invoke-Sninja @($j | Where-Object { $_ })
  Check "$label no-op is clean" ((EdgeCount $r.Out) -eq 0) "got $(EdgeCount $r.Out) edges"

  (Get-Item "$Project\src\a.cpp").LastWriteTime = Get-Date
  $r = Invoke-Sninja @($j | Where-Object { $_ })
  Check "$label rebuilds touched source" ((EdgeCount $r.Out) -ge 3) "got $(EdgeCount $r.Out)"

  $r = Invoke-Sninja @($j | Where-Object { $_ })
  Check "$label settles again" ((EdgeCount $r.Out) -eq 0) "got $(EdgeCount $r.Out)"
}

"--- reference ninja agrees with sninja's logs ---"
& 'C:\choco\bin\ninja.exe' -C "$Project\build" -n 2>&1 | Out-String |
  ForEach-Object { $refOut = $_ }
Check 'reference ninja sees no work' ($refOut -match 'no work to do') $refOut.Trim()

if ($failures -eq 0) { "`nALL CHECKS PASSED" } else { "`n$failures CHECK(S) FAILED" }
exit $failures
