// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exit_status.h"
#include "subprocess.h"

#include <assert.h>
#include <stdio.h>

#include <algorithm>

#include "util.h"
#include "metrics.h"
#ifdef SNINJA_PROFILE_JOBS
#include "json.h"
#include <cerrno>
#include <cstring>
#include <cstdlib>
#endif

using namespace std;

#ifdef SNINJA_PROFILE_JOBS
namespace {
int64_t TraceClock() {
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  return value.QuadPart;
}
uint64_t TraceFileTime(FILETIME time) {
  return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
}

void Subprocess::WriteTrace(DWORD exit_code, bool has_process) {
  if (!trace_.file)
    return;
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  FILETIME creation{}, end{}, kernel{}, user{};
  const bool times_valid = has_process && GetProcessTimes(child_, &creation, &end, &kernel, &user);
  const int64_t finish = TraceClock();
  const std::string command = EncodeJSONString(trace_.command);
  const int okay = fprintf(trace_.file,
      "{\"parent_pid\":%lu,\"pid\":%lu,\"exit_code\":%lu,\"async\":%s,"
      "\"frequency\":%lld,\"queued\":%lld,\"started\":%lld,\"create_begin\":%lld,"
      "\"create_end\":%lld,\"first_output\":%lld,\"finished\":%lld,"
      "\"times_valid\":%s,\"creation_100ns\":%llu,\"exit_100ns\":%llu,"
      "\"kernel_100ns\":%llu,\"user_100ns\":%llu,\"command\":\"%s\"}\n",
      GetCurrentProcessId(), trace_.pid, exit_code, trace_.asynchronous ? "true" : "false",
      static_cast<long long>(frequency.QuadPart), static_cast<long long>(trace_.queued),
      static_cast<long long>(trace_.started), static_cast<long long>(trace_.create_begin),
      static_cast<long long>(trace_.create_end), static_cast<long long>(trace_.first_output),
      static_cast<long long>(finish), times_valid ? "true" : "false",
      static_cast<unsigned long long>(TraceFileTime(creation)),
      static_cast<unsigned long long>(TraceFileTime(end)),
      static_cast<unsigned long long>(TraceFileTime(kernel)),
      static_cast<unsigned long long>(TraceFileTime(user)), command.c_str());
  if (okay < 0 || fflush(trace_.file) != 0)
    Fatal("writing job trace failed: %s", strerror(errno));
  trace_.file = nullptr;
}
#endif

Subprocess::Subprocess(bool use_console) : child_(NULL), pipe_(NULL), overlapped_(),
                                           is_reading_(false),
                                           use_console_(use_console) {
}

Subprocess::~Subprocess() {
  if (pipe_) {
    if (!CloseHandle(pipe_))
      Win32Fatal("CloseHandle");
  }
  // Reap child if forgotten.
  if (child_)
    Finish();
}

HANDLE Subprocess::SetupPipe(HANDLE ioport) {
  char pipe_name[100];
  snprintf(pipe_name, sizeof(pipe_name),
           "\\\\.\\pipe\\ninja_pid%lu_sp%p", GetCurrentProcessId(), this);

  pipe_ = ::CreateNamedPipeA(pipe_name,
                             PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE,
                             PIPE_UNLIMITED_INSTANCES,
                             0, 0, INFINITE, NULL);
  if (pipe_ == INVALID_HANDLE_VALUE)
    Win32Fatal("CreateNamedPipe");

  if (!CreateIoCompletionPort(pipe_, ioport, (ULONG_PTR)this, 0))
    Win32Fatal("CreateIoCompletionPort");

  memset(&overlapped_, 0, sizeof(overlapped_));
  if (!ConnectNamedPipe(pipe_, &overlapped_) &&
      GetLastError() != ERROR_IO_PENDING) {
    Win32Fatal("ConnectNamedPipe");
  }

  // Get the write end of the pipe as a handle inheritable across processes.
  SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
  HANDLE output_write_handle =
      CreateFileA(pipe_name, GENERIC_WRITE, 0, &attributes, OPEN_EXISTING, 0, NULL);
  if (output_write_handle == INVALID_HANDLE_VALUE)
    Win32Fatal("CreateFileA");

  return output_write_handle;
}

bool Subprocess::Start(SubprocessSet* set, string& command) {
  METRIC_RECORD("process start");
#ifdef SNINJA_PROFILE_JOBS
  if (trace_.file)
    trace_.started = TraceClock();
#endif
  HANDLE child_pipe = SetupPipe(set->ioport_);

  STARTUPINFOA startup_info;
  memset(&startup_info, 0, sizeof(startup_info));
  startup_info.cb = sizeof(STARTUPINFO);
  if (!use_console_) {
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = set->nul_;
    startup_info.hStdOutput = child_pipe;
    startup_info.hStdError = child_pipe;
  }
  // In the console case, child_pipe is still inherited by the child and closed
  // when the subprocess finishes, which then notifies ninja.

  PROCESS_INFORMATION process_info;
  memset(&process_info, 0, sizeof(process_info));

  // Ninja handles ctrl-c, except for subprocesses in console pools.
  DWORD process_flags = use_console_ ? 0 : CREATE_NEW_PROCESS_GROUP;

  // Do not prepend 'cmd /c' on Windows, this breaks command
  // lines greater than 8,191 chars.
#ifdef SNINJA_PROFILE_JOBS
  if (trace_.file)
    trace_.create_begin = TraceClock();
#endif
  if (!CreateProcessA(NULL, command.data(), NULL, NULL,
                      /* inherit handles */ TRUE, process_flags,
                      NULL, NULL,
                      &startup_info, &process_info)) {
    DWORD error = GetLastError();
#ifdef SNINJA_PROFILE_JOBS
    if (trace_.file)
      trace_.create_end = TraceClock();
#endif
    if (error == ERROR_FILE_NOT_FOUND) {
      // File (program) not found error is treated as a normal build
      // action failure.
      if (child_pipe)
        CloseHandle(child_pipe);
      CloseHandle(pipe_);
      pipe_ = NULL;
      // child_ is already NULL;
      buf_ = "CreateProcess failed: The system cannot find the file "
          "specified.\n";
      return true;
    } else {
      fprintf(stderr, "\nCreateProcess failed. Command attempted:\n\"%s\"\n",
              command.c_str());
      const char* hint = NULL;
      // ERROR_INVALID_PARAMETER means the command line was formatted
      // incorrectly. This can be caused by a command line being too long or
      // leading whitespace in the command. Give extra context for this case.
      if (error == ERROR_INVALID_PARAMETER) {
        if (command.length() > 0 && (command[0] == ' ' || command[0] == '\t'))
          hint = "command contains leading whitespace";
        else
          hint = "is the command line too long?";
      }
      Win32Fatal("CreateProcess", hint);
    }
  }

#ifdef SNINJA_PROFILE_JOBS
  if (trace_.file) {
    trace_.create_end = TraceClock();
    trace_.pid = process_info.dwProcessId;
  }
#endif

  // Close pipe channel only used by the child.
  if (child_pipe)
    CloseHandle(child_pipe);

  CloseHandle(process_info.hThread);
  child_ = process_info.hProcess;

  return true;
}

void Subprocess::OnPipeReady() {
  DWORD bytes;
  if (!GetOverlappedResult(pipe_, &overlapped_, &bytes, TRUE)) {
    if (GetLastError() == ERROR_BROKEN_PIPE) {
      CloseHandle(pipe_);
      pipe_ = NULL;
      return;
    }
    Win32Fatal("GetOverlappedResult");
  }

  if (is_reading_ && bytes) {
#ifdef SNINJA_PROFILE_JOBS
    if (trace_.file && !trace_.first_output)
      trace_.first_output = TraceClock();
#endif
    buf_.append(overlapped_buf_, bytes);
  }

  memset(&overlapped_, 0, sizeof(overlapped_));
  is_reading_ = true;
  if (!::ReadFile(pipe_, overlapped_buf_, sizeof(overlapped_buf_),
                  &bytes, &overlapped_)) {
    if (GetLastError() == ERROR_BROKEN_PIPE) {
      CloseHandle(pipe_);
      pipe_ = NULL;
      return;
    }
    if (GetLastError() != ERROR_IO_PENDING)
      Win32Fatal("ReadFile");
  }

  // Even if we read any bytes in the readfile call, we'll enter this
  // function again later and get them at that point.
}

ExitStatus Subprocess::Finish() {
  while (!startup_complete_.load(std::memory_order_acquire))
    SwitchToThread();
  if (!child_) {
#ifdef SNINJA_PROFILE_JOBS
    WriteTrace(1, false);
#endif
    return ExitFailure;
  }

  // TODO: add error handling for all of these.
  WaitForSingleObject(child_, INFINITE);

  DWORD exit_code = 0;
  GetExitCodeProcess(child_, &exit_code);
#ifdef SNINJA_PROFILE_JOBS
  WriteTrace(exit_code, true);
#endif

  CloseHandle(child_);
  child_ = NULL;

  return exit_code == CONTROL_C_EXIT ? ExitInterrupted :
                                       static_cast<ExitStatus>(exit_code);
}

bool Subprocess::Done() const {
  return launch_notified_ && connect_seen_ && pipe_ == NULL;
}

const string& Subprocess::GetOutput() const {
  return buf_;
}

HANDLE SubprocessSet::ioport_;

SubprocessSet::SubprocessSet() {
  SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), NULL, TRUE};
  nul_ = CreateFileA("NUL", GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      &attributes, OPEN_EXISTING, 0, NULL);
  if (nul_ == INVALID_HANDLE_VALUE)
    Fatal("couldn't open nul");
  ioport_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
  if (!ioport_)
    Win32Fatal("CreateIoCompletionPort");
  if (!SetConsoleCtrlHandler(NotifyInterrupted, TRUE))
    Win32Fatal("SetConsoleCtrlHandler");
#ifdef SNINJA_PROFILE_JOBS
  if (const char* path = std::getenv("SNINJA_JOB_TRACE")) {
    if (*path) {
      const std::string filename = std::string(path) + "." + std::to_string(GetCurrentProcessId()) + ".jsonl";
      trace_file_ = FOpenNonInheritable(filename.c_str(), "ab");
      if (!trace_file_)
        Fatal("opening job trace failed: %s", strerror(errno));
    }
  }
#endif
}

SubprocessSet::~SubprocessSet() {
  Clear();
#ifdef SNINJA_PROFILE_JOBS
  if (trace_file_ && fclose(trace_file_) != 0)
    Fatal("closing job trace failed: %s", strerror(errno));
#endif

  SetConsoleCtrlHandler(NotifyInterrupted, FALSE);
  CloseHandle(ioport_);
  CloseHandle(nul_);
}

void SubprocessSet::StartLauncher() {
  if (launcher_.joinable())
    return;
  launch_stop_ = false;
  launcher_ = std::thread([this] {
    for (;;) {
      LaunchRequest request;
      {
        std::unique_lock lock(launch_mutex_);
        launch_condition_.wait(lock, [this] { return launch_stop_ || !launch_queue_.empty(); });
        if (launch_stop_)
          return;
        request = std::move(launch_queue_.front());
        launch_queue_.pop();
        launcher_busy_ = true;
      }
      Subprocess* process = request.process;
      process->Start(this, request.command);
      process->startup_complete_.store(true, std::memory_order_release);
      {
        // Start has closed its inheritable writer. A sparse foreground launch
        // can now run safely without inheriting any other job's pipe endpoint.
        std::lock_guard lock(launch_mutex_);
        launcher_busy_ = false;
      }
      // No access to process is permitted after publishing this notification.
      if (!PostQueuedCompletionStatus(ioport_, 0, reinterpret_cast<ULONG_PTR>(process), NULL))
        Win32Fatal("PostQueuedCompletionStatus");
    }
  });
}

void SubprocessSet::StopLauncher() {
  if (!launcher_.joinable())
    return;
  {
    std::lock_guard lock(launch_mutex_);
    launch_stop_ = true;
    std::queue<LaunchRequest>().swap(launch_queue_);
  }
  launch_condition_.notify_one();
  launcher_.join();
}

BOOL WINAPI SubprocessSet::NotifyInterrupted(DWORD dwCtrlType) {
  if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
    if (!PostQueuedCompletionStatus(ioport_, 0, 0, NULL))
      Win32Fatal("PostQueuedCompletionStatus");
    return TRUE;
  }

  return FALSE;
}

Subprocess *SubprocessSet::Add(string command, bool use_console) {
  Subprocess *subprocess = new Subprocess(use_console);
#ifdef SNINJA_PROFILE_JOBS
  if (trace_file_) {
    subprocess->trace_.file = trace_file_;
    subprocess->trace_.queued = TraceClock();
    subprocess->trace_.command = command;
  }
#endif
  // There is nothing to overlap on an empty frontier. In particular, long
  // serial dependency chains should never pay for a launcher handoff.
  bool launch_async = false;
  if (async_launch_ && !running_.empty()) {
    std::lock_guard lock(launch_mutex_);
    // Do not wake an idle thread for an isolated command after a long child
    // computation. Handoffs are useful when launches form a continuous burst.
    launch_async = launcher_busy_ || !launch_queue_.empty() ||
                   GetTimeMillis() - last_dispatch_end_ < 2;
  }
  subprocess->running_index_ = running_.size();
  running_.push_back(subprocess);
#ifdef SNINJA_PROFILE_JOBS
  subprocess->trace_.asynchronous = launch_async;
#endif
  if (launch_async) {
    subprocess->launch_notified_ = false;
    StartLauncher();
    {
      std::lock_guard lock(launch_mutex_);
      launch_queue_.push({subprocess, std::move(command)});
    }
    launch_condition_.notify_one();
  } else {
    subprocess->Start(this, command);
    subprocess->startup_complete_.store(true, std::memory_order_release);
  }
  last_dispatch_end_ = GetTimeMillis();
  return subprocess;
}

SubprocessSet::WorkResult SubprocessSet::DoWork() {
  METRIC_RECORD("process wait");
  DWORD bytes_read;
  Subprocess* subproc;
  OVERLAPPED* overlapped;
  WorkResult work_result = WorkResult::NoWork;

  if (!GetQueuedCompletionStatus(ioport_, &bytes_read, (PULONG_PTR)&subproc,
                                 &overlapped, INFINITE)) {
    if (GetLastError() != ERROR_BROKEN_PIPE)
      Win32Fatal("GetQueuedCompletionStatus");
  }

  if (!subproc) // A NULL subproc indicates that we were interrupted and is
                // delivered by NotifyInterrupted above.
    return WorkResult::Interrupted;

  if (!overlapped) {
    // A launcher publication, not a pipe I/O completion. The first connect
    // packet may have arrived while CreateProcess was still running.
    // The notification was posted after the release store. The acquire is
    // required even in release builds; it must not be hidden in assert().
    while (!subproc->startup_complete_.load(std::memory_order_acquire))
      SwitchToThread();
    subproc->launch_notified_ = true;
  } else {
    subproc->connect_seen_ = true;
  }
  if (!subproc->launch_notified_ || !subproc->connect_seen_)
    return WorkResult::NoWork;

  if (subproc->pipe_)
    subproc->OnPipeReady();

  if (subproc->Done()) {
    const size_t index = subproc->running_index_;
    assert(index < running_.size() && running_[index] == subproc);
    running_[index] = running_.back();
    running_[index]->running_index_ = index;
    running_.pop_back();
    finished_.push(subproc);
    work_result = WorkResult::SubprocFinished;
  }

  return work_result;
}

Subprocess* SubprocessSet::NextFinished() {
  if (finished_.empty())
    return NULL;
  Subprocess* subproc = finished_.front();
  finished_.pop();
  return subproc;
}

void SubprocessSet::Clear() {
  StopLauncher();
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    // Since the foreground process is in our process group, it will receive a
    // CTRL_C_EVENT or CTRL_BREAK_EVENT at the same time as us.
    if ((*i)->child_ && !(*i)->use_console_) {
      if (!GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT,
                                    GetProcessId((*i)->child_))) {
        Win32Fatal("GenerateConsoleCtrlEvent");
      }
    }
  }
  unsigned pending_packets = 0;
  for (Subprocess* process : running_) {
    // StopLauncher joined the only producer. A cancelled queued launch has
    // never submitted any I/O; every started launch has exactly one connect
    // or read operation, unless its connect packet was deferred by DoWork.
    if (process->startup_complete_.load(std::memory_order_acquire)) {
      process->cleanup_packets_ = !process->launch_notified_;
      if (!process->connect_seen_ || (process->launch_notified_ && process->pipe_))
        ++process->cleanup_packets_;
      pending_packets += process->cleanup_packets_;
    }
    if (process->pipe_)
      CancelIoEx(process->pipe_, &process->overlapped_);
  }

  // IOCP owns completion publication. GetOverlappedResult(TRUE) can deadlock
  // before those packets are consumed, so drain the exact outstanding set.
  // No operation is rearmed while aborting, and all contexts stay alive.
  while (pending_packets) {
    DWORD bytes;
    ULONG_PTR key;
    OVERLAPPED* overlapped;
    const BOOL okay = GetQueuedCompletionStatus(ioport_, &bytes, &key, &overlapped, INFINITE);
    if (!okay && !key)
      Win32Fatal("GetQueuedCompletionStatus");
    if (!key)  // A simultaneous console interruption.
      continue;
    auto* process = reinterpret_cast<Subprocess*>(key);
    assert(process->cleanup_packets_ > 0);
    --process->cleanup_packets_;
    --pending_packets;
  }
  for (Subprocess* process : running_) {
    if (process->pipe_) {
      CloseHandle(process->pipe_);
      process->pipe_ = NULL;
    }
    if (process->child_)
      process->Finish();
    delete process;
  }
  running_.clear();
}
