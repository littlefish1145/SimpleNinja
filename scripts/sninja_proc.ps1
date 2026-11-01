# Helper for driving sninja from PowerShell tests and diagnostics.
#
# Dot-source this:  . "$PSScriptRoot\sninja_proc.ps1"
#
# Provides:
#   Start-SninjaProcess   - start sninja in its own process group, return pid
#   Stop-SninjaProcess    - TerminateProcess, i.e. abruptly, with no chance to flush
#   Send-CtrlBreakTo      - deliver a real CTRL_BREAK_EVENT to a process group
#   Wait-ProcessGone      - wait for a pid to disappear, with a timeout
#
# The process group matters: sninja creates its children with
# CREATE_NEW_PROCESS_GROUP, so a console control event aimed at group 0 would also
# hit this script.  Starting sninja as its own group leader lets an event be
# delivered to it alone.

if (-not ('SninjaProc' -as [type])) {
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class SninjaProc {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct STARTUPINFO {
    public int cb;
    public string lpReserved, lpDesktop, lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars;
    public int dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2;
    public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }

  [StructLayout(LayoutKind.Sequential)]
  public struct PROCESS_INFORMATION {
    public IntPtr hProcess, hThread;
    public int dwProcessId, dwThreadId;
  }

  const uint CREATE_NEW_PROCESS_GROUP = 0x00000200;
  const int  STARTF_USESHOWWINDOW    = 0x00000100;
  const short SW_HIDE               = 0;
  const uint PROCESS_TERMINATE      = 0x0001;
  const uint SYNCHRONIZE            = 0x00100000;

  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool CreateProcessW(string app, StringBuilder cmd,
      IntPtr procAttr, IntPtr threadAttr, bool inherit, uint flags,
      IntPtr env, string cwd, ref STARTUPINFO si, out PROCESS_INFORMATION pi);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool TerminateProcess(IntPtr handle, uint exitCode);

  // Parameter names cannot be `event` or `group` -- both are C# keywords.
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent,
                                                      uint processGroupId);

  [DllImport("kernel32.dll")]
  static extern bool CloseHandle(IntPtr handle);

  // Start |exe| as the leader of a new process group.  Showing no window keeps
  // test runs from flashing consoles on the desktop.
  public static int Start(string exe, string args, string cwd) {
    var si = new STARTUPINFO();
    si.cb = Marshal.SizeOf(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi;
    var cmd = new StringBuilder("\"" + exe + "\" " + args);
    if (!CreateProcessW(exe, cmd, IntPtr.Zero, IntPtr.Zero, false,
                        CREATE_NEW_PROCESS_GROUP, IntPtr.Zero, cwd, ref si, out pi))
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pi.dwProcessId;
  }

  // Abrupt.  TerminateProcess gives the target no opportunity to run cleanup, run
  // atexit handlers, or flush anything -- which is exactly the situation the
  // timeline's periodic flush has to survive.
  public static void Kill(int pid) {
    IntPtr h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, false, pid);
    if (h == IntPtr.Zero) return;
    TerminateProcess(h, 9);
    CloseHandle(h);
  }
}
'@ -Language CSharp
}

function Start-SninjaProcess([string]$Exe, [string]$Arguments, [string]$WorkingDirectory) {
  return [SninjaProc]::Start($Exe, $Arguments, $WorkingDirectory)
}

function Stop-SninjaProcess([int]$ProcessId) {
  [SninjaProc]::Kill($ProcessId)
}

function Send-CtrlBreakTo([int]$ProcessGroupId) {
  # 1 = CTRL_BREAK_EVENT.  CTRL_C_EVENT (0) can only be broadcast to group 0, which
  # would signal this script too.
  return [SninjaProc]::GenerateConsoleCtrlEvent(1, [uint32]$ProcessGroupId)
}

function Wait-ProcessGone([int]$ProcessId, [int]$TimeoutMs = 30000) {
  $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
  while ((Get-Date) -lt $deadline) {
    if (-not (Get-Process -Id $ProcessId -EA SilentlyContinue)) { return $true }
    Start-Sleep -Milliseconds 200
  }
  return -not (Get-Process -Id $ProcessId -EA SilentlyContinue)
}
