# Timeline durability under an abrupt kill.
#
# The periodic flush exists for one reason: a build that hangs or is killed must
# still leave a timeline on disk.  That makes the published file's durability the
# property under test -- a snapshot that is half written, out of order, or missing
# its header would make the timeline useless in exactly the situation it was built
# for.
#
# Two things are checked:
#
#   1. With periodic flushing on, killing sninja mid-build leaves a readable
#      snapshot behind.
#   2. Without it, the same kill leaves nothing -- which proves check 1 is
#      observing the periodic flush rather than the exit path doing the work.
#
# The second check matters for the same reason the interrupt test has a negative
# control: "the file was there" is only evidence if "the file was not there"
# is reachable.

$ErrorActionPreference = 'Stop'
$sninja = (Resolve-Path "$PSScriptRoot\..\build\sninja.exe").Path
$py = 'python'
$analyse = Join-Path $PSScriptRoot 'timeline.py'
if (-not (Test-Path $sninja)) { throw "sninja.exe not built" }

$envDump = cmd /c '"D:\vs2022\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && set'
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') {
    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
  }
}

$root = Join-Path $env:TEMP "sninja_timeline_kill_test"
if (Test-Path $root) { Remove-Item -Recurse -Force $root }
New-Item -ItemType Directory -Path $root | Out-Null

# Long enough that the build cannot finish during the test, so the kill always
# lands mid-build.  Same reasoning as the interrupt test: a helper that could exit
# on its own would make "the process is gone" prove nothing.
$src = Join-Path $root "slow.cpp"
@'
#include <windows.h>
int main() { Sleep(600000); return 0; }
'@ | Set-Content -LiteralPath $src -Encoding ASCII
$helper = Join-Path $root "slow.exe"
& cl.exe /nologo /O1 /Fe:"$helper" "$src" 2>&1 | Out-Null
if (-not (Test-Path $helper)) { throw "could not build the test helper" }

$edges = 1..8 | ForEach-Object { "build out$_.txt: slow" }
@"
rule slow
  command = $helper
  description = SLOW %out

$($edges -join "`n")
"@ | Set-Content -LiteralPath (Join-Path $root "build.ninja") -Encoding ASCII

Add-Type -TypeDefinition @'
using System;using System.Runtime.InteropServices;using System.Text;
public static class SninjaKillTest {
  [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] public struct SI {
    public int cb; public string lpReserved, lpDesktop, lpTitle;
    public int dwX,dwY,dwXSize,dwYSize,dwXCountChars,dwYCountChars;
    public int dwFillAttribute,dwFlags; public short wShowWindow,cbReserved2;
    public IntPtr lpReserved2,hStdInput,hStdOutput,hStdError; }
  [StructLayout(LayoutKind.Sequential)] public struct PI {
    public IntPtr hProcess,hThread; public int dwProcessId,dwThreadId; }
  [DllImport("kernel32.dll",SetLastError=true,CharSet=CharSet.Unicode)]
  static extern bool CreateProcessW(string a,StringBuilder c,IntPtr d,IntPtr e,
      bool f,uint g,IntPtr h,string i,ref SI j,out PI k);
  [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint a,bool i,int p);
  [DllImport("kernel32.dll",SetLastError=true)] static extern bool TerminateProcess(IntPtr h,uint c);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

  // CREATE_NEW_PROCESS_GROUP, so this test's own console events do not reach it.
  public static int Start(string exe,string args,string cwd,out int pid) {
    var si=new SI(); si.cb=Marshal.SizeOf(si); si.dwFlags=0x100; si.wShowWindow=0;
    PI pi; var cmd=new StringBuilder("\""+exe+"\" "+args);
    if(!CreateProcessW(exe,cmd,IntPtr.Zero,IntPtr.Zero,false,0x200,IntPtr.Zero,cwd,ref si,out pi))
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); pid=pi.dwProcessId; return pid; }

  // Abrupt: no chance to flush, no atexit handler, no orderly shutdown.  This is
  // the whole point -- TerminateProcess gives the process no opportunity to write
  // anything on its way out.
  public static void Kill(int pid) {
    IntPtr h=OpenProcess(0x0001|0x0400,false,pid);
    if(h!=IntPtr.Zero){ TerminateProcess(h,9); CloseHandle(h); } }
}
'@ -Language CSharp

$fail = 0
function Assert($ok, $what) {
  if ($ok) { Write-Host "  PASS  $what" } else { Write-Host "  FAIL  $what"; $script:fail++ }
}
function Cleanup {
  Get-Process -Name 'slow' -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
}

function Test-CsvReadable([string]$path) {
  # Structural check: header present, every row has five fields, and the times are
  # non-decreasing.  A truncated or interleaved file fails one of these.
  if (-not (Test-Path $path)) { return "missing" }
  $lines = @(Get-Content $path)
  if ($lines.Count -eq 0) { return "empty" }
  if (-not $lines[0].StartsWith("t_ms,kind,worker,edge_key,edge")) { return "bad header" }
  $last = -1.0
  $rows = 0
  for ($i = 1; $i -lt $lines.Count; $i++) {
    $parts = $lines[$i].Split(',', 5)
    if ($parts.Count -lt 5) { return "row $i has $($parts.Count) fields, expected 5" }
    $t = 0.0
    if (-not [double]::TryParse($parts[0], [ref]$t)) { return "row $i has a bad timestamp" }
    if ($t -lt $last) { return "row $i goes backwards in time ($t < $last)" }
    $last = $t
    $rows++
  }
  if ($rows -eq 0) { return "no data rows" }
  return "ok ($rows rows, $([math]::Round($last,0)) ms)"
}

function Invoke-KillCase([string]$label, [hashtable]$envVars, [int]$killAfterMs) {
  foreach ($k in $envVars.Keys) { Set-Item -Path "Env:$k" -Value $envVars[$k] }
  $csv = Join-Path $root "$label.csv"
  Remove-Item -Force $csv -EA SilentlyContinue
  try {
    Cleanup
    Remove-Item -Force (Join-Path $root "out*.txt") -EA SilentlyContinue
    $env:SNINJA_TIMELINE = $csv

    $pid1 = 0
    [SninjaKillTest]::Start($sninja, "-C `"$root`" -j 4", $root, [ref]$pid1) | Out-Null
    Start-Sleep -Milliseconds $killAfterMs
    [SninjaKillTest]::Kill($pid1)

    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and (Get-Process -Id $pid1 -EA SilentlyContinue)) {
      Start-Sleep -Milliseconds 100
    }
    $survived = [bool](Get-Process -Id $pid1 -EA SilentlyContinue)
    # Give the OS a moment to settle any file handles from the dead process.
    Start-Sleep -Milliseconds 500
    return @{ Pid = $pid1; Survived = $survived; Csv = $csv }
  } finally {
    foreach ($k in $envVars.Keys) { Remove-Item "Env:$k" -EA SilentlyContinue }
    Remove-Item Env:\SNINJA_TIMELINE -EA SilentlyContinue
    Cleanup
  }
}

Write-Host "=== timeline durability under an abrupt kill ==="

# 1. Periodic flushing on: the snapshot must survive the kill.
$r = Invoke-KillCase "flushon" @{ SNINJA_TIMELINE_FLUSH_MS = "200" } 2500
Assert (-not $r.Survived) "sninja was killed mid-build (pid $($r.Pid))"
$verdict = Test-CsvReadable $r.Csv
Assert ($verdict -eq "ok ($($verdict -replace '^ok \(','' -replace '\)$','')" -or $verdict -like "ok (*") `
  "snapshot left by the killed process is complete and readable: $verdict"

# 2. No periodic flushing: nothing should be published, because the exit path never
#    runs.  Without this, check 1 could be passing on the strength of the exit path
#    rather than the periodic flush.
$r2 = Invoke-KillCase "flushoff" @{} 2500
Assert (-not $r2.Survived) "sninja was killed mid-build with flushing off (pid $($r2.Pid))"
Assert (-not (Test-Path $r2.Csv)) "no timeline published when the process was killed before exiting"

# 3. No orphaned temporaries.  The atomic-rename scheme writes
#    <target>.tmp.<pid>.<n>; a reader must never see one of those as the timeline.
$strays = @(Get-ChildItem $root -Filter "*.tmp.*" -EA SilentlyContinue)
Assert ($strays.Count -eq 0) "no leftover temporary files ($($strays.Count) found)"
foreach ($s in $strays) { Write-Host "          $($s.Name)" }

# 4. The snapshot the killed process left must actually be analysable, not merely
#    well-formed: this is the tool's output being used for its purpose.
if (Test-Path $r.Csv) {
  $out = & $py $analyse $r.Csv 2>&1
  Assert ($LASTEXITCODE -eq 0) "timeline.py accepts the snapshot from the killed process"
  $summary = ($out | Select-String -Pattern "^\s+edges\s+:").Line
  if ($summary) { Write-Host "          $summary".TrimEnd() }
}

Write-Host "=== timeline kill test: $(if ($fail -eq 0) { 'ALL TIMELINE KILL CHECKS PASSED' } else { "$fail CHECK(S) FAILED" }) ==="
if ($fail -ne 0) { exit 1 }
