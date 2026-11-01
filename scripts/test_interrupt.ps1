# Ctrl-C / console interrupt.
#
# The property under test is that an interrupted build leaves no children behind.
# With CREATE_NEW_PROCESS_GROUP a console Ctrl-C does not reach the compilers at
# all, so before the handler and the job object existed, Ctrl-C left them running
# unsupervised.  These tests exist to keep that from coming back.
#
# The signal is a real CTRL_BREAK_EVENT, delivered to sninja's own process group.
# That is deliberate: CTRL_C_EVENT can only be broadcast to group 0 (which would
# also hit this test script), while CTRL_BREAK_EVENT can be aimed at one group.
# sninja's children are in their own groups, so they receive nothing -- exactly
# the situation where sninja alone has to notice and clean up.

$ErrorActionPreference = 'Stop'
$sninja = (Resolve-Path "$PSScriptRoot\..\build\sninja.exe").Path
if (-not (Test-Path $sninja)) { throw "sninja.exe not built" }

$envDump = cmd /c '"D:\vs2022\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 && set'
foreach ($line in $envDump) {
  if ($line -match '^([^=]+)=(.*)$') {
    [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
  }
}

$root = Join-Path $env:TEMP "sninja_interrupt_test"
if (Test-Path $root) { Remove-Item -Recurse -Force $root }
New-Item -ItemType Directory -Path $root | Out-Null

# A command that outlives the interrupt if nobody kills it, named distinctly so
# the test can count exactly the processes it started.
#
# The sleep is far longer than any wait in this script on purpose: if the helper
# finished on its own, "no leaked processes" would prove nothing.  A negative
# control confirmed that -- with a 30 s sleep and a 30 s wait, the leaked count
# came back zero whether or not sninja cleaned up.
$src = Join-Path $root "slow.cpp"
@'
#include <windows.h>
int main() {
  Sleep(600000);
  return 0;
}
'@ | Set-Content -LiteralPath $src -Encoding ASCII
$exe = Join-Path $root "slow.exe"
& cl.exe /nologo /O1 /Fe:"$exe" "$src" 2>&1 | Out-Null
if (-not (Test-Path $exe)) { throw "could not build the test helper $exe" }

# Four independent edges, so the interrupt lands on a build with work genuinely in
# flight.  Each output is a distinct file: sninja would otherwise collapse a rule
# with several outputs into a single edge, and there would be nothing to interrupt.
@"
rule slow
  command = $exe
  description = SLOW %out

build out0.txt: slow
build out1.txt: slow
build out2.txt: slow
build out3.txt: slow
"@ | Set-Content -LiteralPath (Join-Path $root "build.ninja") -Encoding ASCII

$helper = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class SninjaTest {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct STARTUPINFO {
    public int cb; public string lpReserved, lpDesktop, lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars;
    public int dwFillAttribute, dwFlags; public short wShowWindow, cbReserved2;
    public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }
  [StructLayout(LayoutKind.Sequential)]
  public struct PROCESS_INFORMATION {
    public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId;
  }

  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool CreateProcessW(string app, StringBuilder cmd,
      IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string cwd,
      ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern bool GenerateConsoleCtrlEvent(uint ev, uint group);
  [DllImport("kernel32.dll")]
  public static extern bool CloseHandle(IntPtr h);

  const uint CREATE_NEW_PROCESS_GROUP = 0x00000200;

  // Start sninja as the leader of a new process group, so a console event can be
  // aimed at it alone.
  public static int Start(string exe, string args, string cwd, out int pid) {
    var si = new STARTUPINFO();
    si.cb = Marshal.SizeOf(si);
    si.dwFlags = 0x100; // STARTF_USESHOWWINDOW
    si.wShowWindow = 0;  // SW_HIDE: do not pop a window during tests
    PROCESS_INFORMATION pi;
    var cmd = new StringBuilder("\"" + exe + "\" " + args);
    if (!CreateProcessW(exe, cmd, IntPtr.Zero, IntPtr.Zero, false,
                        CREATE_NEW_PROCESS_GROUP, IntPtr.Zero, cwd,
                        ref si, out pi))
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    pid = pi.dwProcessId;
    return pid;
  }
}
'@
Add-Type -TypeDefinition $helper -Language CSharp

$fail = 0
function Assert($ok, $what) {
  if ($ok) { "  PASS  $what" } else { "  FAIL  $what"; $script:fail++ }
}
function SlowCount { @(Get-Process -Name 'slow' -ErrorAction SilentlyContinue).Count }
function Cleanup { Get-Process -Name 'slow' -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue }

function Invoke-InterruptCase([string]$label, [hashtable]$envVars) {
  # One interrupt scenario.  Returns the number of leaked children, or -1 if the
  # scenario could not be set up (which is itself a failure, not a pass).
  #
  # Diagnostics go to the host, not to the output stream: in PowerShell a plain
  # string inside a function becomes part of its return value, so writing them
  # normally would turn the returned leak count into an array and quietly break
  # every comparison the caller makes.
  foreach ($k in $envVars.Keys) {
    Set-Item -Path "Env:$k" -Value $envVars[$k]
  }
  try {
    Cleanup
    Remove-Item -Force "$root\out*.txt", "$root\.ninja_log" -EA SilentlyContinue

    $sninjaPid = 0
    [SninjaTest]::Start($sninja, "-C `"$root`" -j 4", $root, [ref]$sninjaPid) | Out-Null

    # Wait for a child to actually be running, so we interrupt a live build rather
    # than a process that has not got going yet.
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline -and (SlowCount) -lt 1) { Start-Sleep -Milliseconds 100 }
    if ((SlowCount) -lt 1) {
      Write-Host "    [$label] no child process appeared within 20s (pid $sninjaPid)"
      return -1
    }

    # CTRL_BREAK_EVENT to sninja's group only.  Its compilers are in their own
    # groups, so they get nothing: stopping them is sninja's job.
    if (-not [SninjaTest]::GenerateConsoleCtrlEvent(1, [uint32]$sninjaPid)) {
      Write-Host "    [$label] GenerateConsoleCtrlEvent failed"
      return -1
    }

    $gone = $false
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
      if (-not (Get-Process -Id $sninjaPid -EA SilentlyContinue)) { $gone = $true; break }
      Start-Sleep -Milliseconds 200
    }
    if (-not $gone) { Get-Process -Id $sninjaPid -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue }

    # The property under test: an interrupted build leaves nothing running.
    Start-Sleep -Seconds 2
    $leaked = SlowCount
    Cleanup
    Write-Host "  scenario '$label': sninja exited=$gone, leaked=$leaked"
    return $leaked
  } finally {
    foreach ($k in $envVars.Keys) { Remove-Item "Env:$k" -EA SilentlyContinue }
    Cleanup
  }
}


Cleanup
"=== interrupt tests ==="

# 1. The real configuration: children in the kill-on-close job.
$leaked = Invoke-InterruptCase "with job (default)" @{}
if ($leaked -lt 0) {
  Assert $false "scenario ran far enough to interrupt a live build"
} else {
  Assert $true "child process was running before the interrupt"
  Assert $true "delivered CTRL_BREAK_EVENT to sninja's process group"
  Assert ($leaked -eq 0) "no compiler processes left behind after the interrupt (leaked=$leaked)"
}

# 2. Negative control.  There are two independent mechanisms that stop children on
#    an interrupt -- the kill-on-close job object, and the per-child Terminate in
#    the drain -- so disabling only one of them still leaves the other working.
#    Disabling both makes the same interrupt leak children, which proves the check
#    in step 1 is actually observing the cleanup rather than passing because the
#    interrupt never arrived or the helper was simply too short-lived to notice.
#    If this scenario ever passes, the leak test has stopped testing anything.
$leakedNoCleanup = Invoke-InterruptCase "no job, no child kill" `
  @{ SNINJA_NO_JOB = "1"; SNINJA_NO_CHILD_KILL = "1" }
if ($leakedNoCleanup -ge 0) {
  Assert ($leakedNoCleanup -gt 0) `
    "negative control: with both cleanup mechanisms off the interrupt leaks children (leaked=$leakedNoCleanup)"
} else {
  Assert $false "negative control scenario could not be set up"
}

"=== interrupt tests: $(if ($fail -eq 0) { 'ALL INTERRUPT CHECKS PASSED' } else { "$fail CHECK(S) FAILED" }) ==="
if ($fail -ne 0) { exit 1 }
