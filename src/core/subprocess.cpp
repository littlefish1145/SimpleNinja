#include "subprocess.h"

#include "interrupt.h"
#include "job.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <vector>

#include "graph.h"
#include "util.h"

using namespace std;

Subprocess::~Subprocess() {
  JoinOutputReader();
  if (process_) {
    CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
  }
  if (pipe_read_) {
    CloseHandle(static_cast<HANDLE>(pipe_read_));
    pipe_read_ = nullptr;
  }
}

bool Subprocess::Start(Edge* edge, string* err) {
  command_ = edge->EvaluateCommand();
  if (command_.empty()) {
    *err = "edge has no command";
    return false;
  }

  if (getenv("SNINJA_TRACE_SUBPROCESS")) {
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    fprintf(stderr, "[sninja] cwd=%s\n[sninja] cmd=%s\n", cwd, command_.c_str());
    // Probe: can we ourselves create a file next to the edge's outputs?
    for (const Node* out : edge->outputs_) {
      std::string probe_path = out->path() + ".sninja_probe";
      HANDLE h = CreateFileA(probe_path.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      fprintf(stderr, "[sninja] probe create %s -> %lu\n", probe_path.c_str(),
              h == INVALID_HANDLE_VALUE ? GetLastError() : 0);
      if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        DeleteFileA(probe_path.c_str());
      }
    }
  }

  STARTUPINFOA startup;
  memset(&startup, 0, sizeof(STARTUPINFOA));
  startup.cb = sizeof(STARTUPINFOA);
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  startup.dwFlags = STARTF_USESTDHANDLES;

  HANDLE pipe_write = nullptr;
  if (capture_output_) {
    HANDLE read_end = nullptr;
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&read_end, &pipe_write, &sa, 0)) {
      *err = string("CreatePipe failed: ") + GetLastErrorString();
      return false;
    }
    // The read end must not leak into the child.
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    pipe_read_ = read_end;
    // cl.exe writes /showIncludes to stderr, so both streams must be captured.
    startup.hStdOutput = pipe_write;
    startup.hStdError = pipe_write;
  }

  DWORD flags = 0;
  if (!use_console_pool_) {
    // Background commands must not steal the foreground console, and Ctrl-C must
    // not reach them -- sninja decides what to stop itself.
    //
    // This deliberately mirrors what the reference ninja passes (CREATE_NEW_
    // PROCESS_GROUP) rather than CREATE_NO_WINDOW: measurements on a 120-unit
    // project showed the compiler taking ~27% longer in total under
    // CREATE_NO_WINDOW, which was most of the gap against the reference at
    // low -j.  Re-measure with SNINJA_TIME_PHASES=1 before changing it back.
    flags |= CREATE_NEW_PROCESS_GROUP;
  }


  // CreateProcess takes a single, writable command-line string that includes
  // the executable name.
  vector<char> cmdline(command_.begin(), command_.end());
  cmdline.push_back('\0');

  PROCESS_INFORMATION process;
  memset(&process, 0, sizeof(process));
  // Handles are inherited because the child needs our std handles and, when
  // capturing, the pipe write end.  Everything else this process has open must
  // have been marked non-inheritable at the point it was opened (see
  // util::MakeNonInheritable), otherwise a child would keep e.g. the build log
  // or .ninja_lock file locked after we exit.
  if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, flags,
                      nullptr, nullptr, &startup, &process)) {
    if (pipe_write)
      CloseHandle(pipe_write);
    *err = "CreateProcess failed: " + GetLastErrorString();
    return false;
  }

  if (pipe_write) {
    // The child owns the write end now; closing ours lets the read side see
    // EOF once the child exits.
    CloseHandle(pipe_write);
  }
    CloseHandle(process.hThread);
    process_ = process.hProcess;
    // Put the child in the kill-on-close job, so that however sninja ends --
    // normal exit, Ctrl-C, or being killed outright -- this process and
    // anything it spawned are cleaned up by the OS instead of being orphaned.
    if (JobObjectEnabled())
    KillOnCloseJob::Global()->Assign(process_);
    StartOutputReader();
    return true;
  }


void Subprocess::StartOutputReader() {
  if (!pipe_read_)
    return;
  // Read concurrently with the child's execution.  A pipe holds only a fixed
  // amount of buffer (64 KB by default), so a compiler that emits more output
  // than that would block in write() and never exit -- and if we only read
  // after waiting, the wait would never return.
  HANDLE read_end = static_cast<HANDLE>(pipe_read_);
  output_reader_ = std::thread([this, read_end] {
    std::string captured;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(read_end, buf, sizeof(buf), &n, nullptr) && n > 0)
      captured.append(buf, n);
    output_ = std::move(captured);
  });
}

void Subprocess::JoinOutputReader() {
  if (output_reader_.joinable())
    output_reader_.join();
  if (pipe_read_) {
    CloseHandle(static_cast<HANDLE>(pipe_read_));
    pipe_read_ = nullptr;
  }
}

const string* Subprocess::Wait(string* err) {
  if (!process_) {
    *err = "no subprocess running";
    return err;
  }

  HANDLE handle = static_cast<HANDLE>(process_);
  DWORD wait_result = WaitForSingleObject(handle, INFINITE);
  if (wait_result == WAIT_FAILED) {
    *err = string("waiting for command failed: ") + GetLastErrorString();
    return err;
  }

  // Collect the captured output before reporting the result.
  JoinOutputReader();

  DWORD exit_code = 0;
  if (!GetExitCodeProcess(handle, &exit_code)) {
    *err = string("GetExitCodeProcess failed: ") + GetLastErrorString();
    return err;
  }
  if (exit_code != 0) {
    char buf[64];
    snprintf(buf, sizeof(buf), "exit %lu: ", (unsigned long)exit_code);
    *err = buf + command_;
    return err;
  }

  return nullptr;
}

bool Subprocess::TryFinish(bool* finished, int* exit_code, std::string* err) {
  *finished = false;
  *exit_code = 0;
  if (!process_) {
    // Never started, or already reaped.
    *err = "no subprocess running";
    return false;
  }

  HANDLE handle = static_cast<HANDLE>(process_);
  const DWORD wait_result = WaitForSingleObject(handle, 0);
  if (wait_result == WAIT_TIMEOUT)
    return true;  // still running
  if (wait_result == WAIT_FAILED) {
    *err = string("waiting for command failed: ") + GetLastErrorString();
    return false;
  }

  // Exited: collect the captured output, then read the exit code.
  //
  // The join can in principle outlast the process itself, if a descendant
  // inherited the pipe's write end and is still holding it open.  Measure that
  // rather than assume it.
  const int64_t join_start = NowMillis();
  JoinOutputReader();
  extern void RecordReapMillis(int64_t ms);
  RecordReapMillis(NowMillis() - join_start);

  DWORD code = 0;
  if (!GetExitCodeProcess(handle, &code)) {
    *err = string("GetExitCodeProcess failed: ") + GetLastErrorString();
    return false;
  }
  *finished = true;
  *exit_code = static_cast<int>(code);
  return true;
}

void Subprocess::Terminate() {
  if (process_) {
    // Ask the child to exit, then kill it if it does not.  We do not wait here:
    // the owning worker reaps it through TryFinish().
    HANDLE handle = static_cast<HANDLE>(process_);
    DWORD pid = GetProcessId(handle);
    if (pid) {
      // TerminateJobObject would be cleaner but requires a job per child.
      TerminateProcess(handle, 1);
    }
  }
}

bool Subprocess::Done() const {

  if (!process_)
    return true;
  return WaitForSingleObject(static_cast<HANDLE>(process_), 0) != WAIT_TIMEOUT;
}
