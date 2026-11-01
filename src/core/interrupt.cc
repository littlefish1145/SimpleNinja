#include "interrupt.h"

#include <windows.h>

namespace {

// A manual-reset event: several workers wait on it, and a single Ctrl-C must be
// seen by all of them.
HANDLE g_event = nullptr;
bool g_installed = false;

// Console handlers run on a thread the OS creates, on a deadline, with very
// little that is safe to call.  Setting an event is about the only useful thing:
// it is a kernel call, it does not take a lock that build threads might hold, and
// it does not risk deadlocking against them.
BOOL WINAPI Handler(DWORD type) {
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      if (g_event)
        SetEvent(g_event);
      // Report handled: sninja stops the build itself and reports the failure,
      // which is a better message than the default "process terminated".
      return TRUE;
    default:
      return FALSE;
  }
}

HANDLE Event() {
  if (!g_event) {
    g_event = CreateEventA(nullptr, /*manualReset=*/TRUE,
                           /*initialState=*/FALSE, nullptr);
  }
  return g_event;
}

}  // namespace

bool JobObjectEnabled() {
  return getenv("SNINJA_NO_JOB") == nullptr;
}

void InstallConsoleInterruptHandler() {
  if (g_installed)
    return;
  g_installed = true;
  if (!Event())
    return;
  // The children are in their own process groups, so only this process needs
  // telling.  The flag would only matter for children sharing our group, which
  // is the console pool's interactive commands, and those the user is running
  // deliberately -- so it is left off.
  SetConsoleCtrlHandler(Handler, TRUE);
}

bool InterruptRequested() {
  HANDLE e = Event();
  if (!e)
    return false;
  return WaitForSingleObject(e, 0) == WAIT_OBJECT_0;
}

void* ConsoleInterruptEvent() {
  return Event();
}

void RequestInterrupt() {
  HANDLE e = Event();
  if (e)
    SetEvent(e);
}
