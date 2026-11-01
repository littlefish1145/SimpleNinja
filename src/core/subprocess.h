#ifndef SNINJA_SUBPROCESS_H_
#define SNINJA_SUBPROCESS_H_

#include <string>
#include <thread>

#include "timestamp.h"

struct Edge;

/// A single asynchronous subprocess, running an edge's command.
struct Subprocess {
  /// If |use_console_pool| is true, the subprocess runs attached to the
  /// console (so that it can read from stdin and write to the terminal),
  /// which reserves the console pool while it runs.
  /// If |capture_output| is true, the child's stdout is captured and made
  /// available via output() (used to parse /showIncludes output).
  explicit Subprocess(bool use_console_pool, bool capture_output = false)
      : use_console_pool_(use_console_pool),
        capture_output_(capture_output) {}
  ~Subprocess();

  /// Returns false on failure (with a message in |err|).
  bool Start(Edge* edge, std::string* err);

  /// Wait for the subprocess to finish, and return whether it was successful.
  /// Returns nullptr on success, or an error message.
  const std::string* Wait(std::string* err);

  /// Non-blocking completion check.  Returns false only if the wait itself
  /// failed.  On return |*finished| says whether the command has exited; only
  /// then is |*exit_code| meaningful.
  ///
  /// This is what lets one thread keep several commands in flight instead of
  /// blocking on each in turn: start a command, go and start the next one, and
  /// come back later to reap whatever has finished.
  bool TryFinish(bool* finished, int* exit_code, std::string* err);

  /// True if the subprocess has finished (whether or not it succeeded).
  bool Done() const;

  /// The captured stdout, valid once the command has finished and
  /// capture_output was set.
  const std::string& output() const { return output_; }

  /// The command line that was launched.
  const std::string& command() const { return command_; }

  /// Ask the child to terminate (used when the build is interrupted).
  void Terminate();

  /// The process handle, so a scheduler can wait on several children at once
  /// with WaitForMultipleObjects instead of polling.  Null if never started.
  void* handle() const { return process_; }

 private:
  bool use_console_pool_;
  bool capture_output_;
  void* process_;    // HANDLE
  void* pipe_read_;  // HANDLE, when capturing output
  std::string command_;
  std::string output_;
  /// Drains |pipe_read_| while the command runs.  A child that writes more than
  /// the pipe buffer holds (64 KB by default) would block forever if we only
  /// read after it exited, so the read has to overlap the wait.
  std::thread output_reader_;

  void StartOutputReader();
  void JoinOutputReader();

  Subprocess(const Subprocess&) = delete;
  Subprocess& operator=(const Subprocess&) = delete;
};

#endif  // SNINJA_SUBPROCESS_H_
