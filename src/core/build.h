#ifndef SNINJA_BUILD_H_
#define SNINJA_BUILD_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "graph.h"
#include "state.h"
#include "timestamp.h"

struct BuildLog;
struct DepsLog;
struct DiskInterface;
struct Edge;
struct Node;
struct Pool;
struct StatusPrinter;
struct Subprocess;

/// One command the scheduler has launched but not yet reaped.
///
/// Splitting a command into start / poll / finish is what lets a single worker
/// thread keep several commands in flight.  That matters because a build spends
/// almost all of its wall clock inside child processes: creating one is cheap,
/// waiting for one is not, so a thread that blocks on each command in turn
/// spends most of its time asleep.
struct RunningCommand {
  Edge* edge = nullptr;
  std::unique_ptr<Subprocess> subprocess;
  int64_t start_time = 0;
  /// When the child was first observed to have exited.  Lets the build report
  /// how much of an edge's wall clock went into waiting for the child versus
  /// reaping it, which is what says whether the output pipe is the problem.
  int64_t observed_exit_time = 0;
  int exit_code = 0;
  /// True when the edge uses `deps = msvc`, so its output was captured.
  bool deps_msvc = false;
  /// The pool this command holds a slot in, released when it is reaped.
  struct Pool* pool = nullptr;
  /// Set when the launch (StartEdge) itself failed, as opposed to the command
  /// running and returning non-zero.  Needed because the launch may now happen
  /// on another thread, after the worker has already queued the command.
  bool launch_failed = false;
  /// The launch error, when launch_failed.
  std::string launch_err;
  /// Handle of the spawner event this command is waiting on, or null once the
  /// launch has been harvested.
  void* launch_event = nullptr;
  /// Set when the build is being torn down and this command must not be
  /// launched.  Without it the spawner could call CreateProcess after the drain
  /// has finished, leaving a process nobody will ever reap.  Guarded by the
  /// plan's mutex: the spawner reads it under the same lock the drain takes when
  /// setting it, so a launch can never slip past a teardown.
  bool launch_cancelled = false;
};


/// Command line options affecting the build.
struct BuildConfig {
  BuildConfig()
      : verbosity(NORMAL_ERRORS), dry_run(false), parallelism(1),
        keep_going(false) {}

  enum Verbosity {
    QUIET,          // No output -- used when testing.
    NORMAL_ERRORS,  // Only build errors.
    STATUS,         // Show build progress.
    COMMAND,        // Show command lines as they are run.
  };

  int verbosity;
  bool dry_run;
  int parallelism;
  bool keep_going;
};

/// Builds the targets added with AddTarget().
struct Builder {
  Builder(State* state, const BuildConfig& config, BuildLog* build_log,
          DepsLog* deps_log, DiskInterface* disk_interface,
          StatusPrinter* status, int64_t start_time_millis);

  ~Builder();

  /// Add a target to our build (including all its dependencies).
  /// Returns false on error.
  bool AddTarget(const std::string& target_name, std::string* err);

  /// After all targets have been added, start building them.
  /// Returns false on error (with keep_going, errors are collected and the
  /// build continues as far as possible).
  bool Build(std::string* err);

  /// Clean up after a build, if it was interrupted.
  bool Cleanup(std::string* err);

  /// Path of the build log, if one was configured.
  const std::string& build_log_path() const { return build_log_path_; }

  /// Test hook: point the builder at a deps log to write.
  void SetDepsLogPath(const std::string& path) { deps_log_path_ = path; }

  /// Test hook: load the build log from |log| before building.
  void SetBuildLog(const std::string& log);

  State* state() { return state_; }

  /// Launch |edge|'s command.  Returns false only if the command could not be
  /// started at all; a command that starts and later fails is reported by
  /// FinishEdge.  Public because the scheduler drives it from its workers.
  bool StartEdge(RunningCommand* cmd, Edge* edge, std::string* err);

  /// Non-blocking: has this command exited yet?
  bool PollEdge(RunningCommand* cmd, bool* finished, std::string* err);

  /// Reap a command that PollEdge reported as finished, update the logs and
  /// report whether it succeeded.
  bool FinishEdge(RunningCommand* cmd, std::string* err);

  /// Run one command to completion on this thread.  Used by -n (dry run) and by
  /// the sequential path.
  bool RunEdgeSynchronously(Edge* edge, std::string* err);

  const BuildConfig& config() const { return config_; }

 private:
  /// The filesystem timestamp recorded in the build log for every command.
  /// Computed once per build; see the comment in the implementation for why it
  /// is not simply the wall clock.
  TimeStamp GetBuildStartTime(std::string* err);


  /// Prepare |node_| and everything it depends on, and mark dirty nodes.
  bool PrepareNode(Node* node, std::string* err);

  /// Update the logs with the result of a command.  |subprocess_output| is
  /// the command's captured stdout when it was captured, else nullptr.
  bool FinishCommand(Edge* edge, int start_time, int end_time, bool success,
                     std::string* err, const std::string* subprocess_output = nullptr);

  /// Parse the deps discovered by a finished command into |deps_nodes|.
  /// |subprocess_output| is the command's captured stdout (used by "msvc").
  bool ExtractDeps(Edge* edge, const std::string& deps_type,
                   std::vector<Node*>* deps_nodes,
                   const std::string& subprocess_output, std::string* err);

  /// Write the response file for an edge, if it has one.
  bool WriteRspFile(Edge* edge, std::string* err);

  /// Called when an edge fails; prints the error and returns whether the
  /// build should keep going.
  bool ReportBuildFailure(Edge* edge, const std::string& err, std::string* err_out);

  int64_t GetTimeMillis() const;

  State* state_;
  const BuildConfig& config_;
  DiskInterface* disk_interface_;
  DepsLog* deps_log_;
  BuildLog* build_log_;
  StatusPrinter* status_;
  int64_t start_time_millis_;

  std::string build_log_path_;
  std::string deps_log_path_;
  std::string log_err_;
  std::string lock_file_path_;
  /// Cached result of writing and stat()ing |lock_file_path_|; -1 until first use.
  TimeStamp build_start_time_ = -1;
  /// Serialises the post-command bookkeeping (logs, node stats), which several
  /// worker threads would otherwise race on.
  std::mutex bookkeeping_mutex_;

  /// Edges to run, in dependency order (populated by AddTarget).
  std::vector<Edge*> ready_;

  /// Dependency scanner used by AddTarget.
  std::unique_ptr<DependencyScan> scan_;

  /// Loads deps from depfiles / the deps log.
  std::unique_ptr<DepsLoader> deps_loader_;

  Builder(const Builder&) = delete;
  Builder& operator=(const Builder&) = delete;
};

#endif  // SNINJA_BUILD_H_
