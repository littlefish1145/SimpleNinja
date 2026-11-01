#include "build.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <set>

#include "build_log.h"
#include "clparser.h"
#include "plan.h"
#include "depfile_parser.h"
#include "deps_loader.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "graph.h"
#include "status_printer.h"
#include "subprocess.h"
#include "util.h"

using namespace std;

namespace {

/// True if any output of |edge| was marked dirty by the dependency scan.
bool EdgeIsDirty(const Edge* edge) {
  for (const Node* out : edge->outputs_) {
    if (out->dirty())
      return true;
  }
  return false;
}

/// Post-order DFS over the producing edges reachable from |node|, appending
/// every dirty edge exactly once.  Post-order guarantees an edge's inputs are
/// appended before the edge itself, so a single pass can execute them.
void CollectDirtyEdges(Node* node, std::set<Edge*>* seen,
                       std::vector<Edge*>* out) {
  Edge* edge = node->in_edge();
  if (!edge)
    return;
  if (!seen->insert(edge).second)
    return;
  for (Node* in : edge->inputs_)
    CollectDirtyEdges(in, seen, out);
  if (EdgeIsDirty(edge))
    out->push_back(edge);
}

}  // namespace

Builder::Builder(State* state, const BuildConfig& config, BuildLog* build_log,
                 DepsLog* deps_log, DiskInterface* disk_interface,
                 StatusPrinter* status, int64_t start_time_millis)
    : state_(state), config_(config), disk_interface_(disk_interface),
      deps_log_(deps_log), build_log_(build_log), status_(status),
      start_time_millis_(start_time_millis) {
  deps_loader_ = std::make_unique<ImplicitDepLoader>(state_, deps_log_,
                                                    disk_interface_);
  scan_ = std::make_unique<DependencyScan>(build_log_, disk_interface_,
                                          deps_loader_.get());
  lock_file_path_ = ".ninja_lock";
  std::string build_dir = state_->bindings_.LookupVariable("builddir");
  if (!build_dir.empty())
    lock_file_path_ = build_dir + "/" + lock_file_path_;
}


Builder::~Builder() = default;

int64_t Builder::GetTimeMillis() const {
  return NowMillis() - start_time_millis_;
}

void Builder::SetBuildLog(const std::string& log) {
  if (log.empty())
    return;
  build_log_->Load(log, &log_err_);
  build_log_path_ = log;
}

bool Builder::AddTarget(const string& target_name, string* err) {
  // Targets on the command line may be dirty paths that are not in the graph.
  string path = target_name;
  uint64_t slash_bits;
  CanonicalizePath(&path, &slash_bits);
  Node* node = state_->LookupNode(path);
  if (!node) {
    *err = "unknown target: '" + target_name + "'";
    return false;
  }

  // Mark the target and its transitive inputs as clean or dirty.
  std::vector<Node*> validation_nodes;
  if (!scan_->RecomputeDirty(node, &validation_nodes, err))
    return false;

  // Validation nodes must exist before the build starts.
  for (Node* validation : validation_nodes) {
    if (!validation->StatIfNecessary(disk_interface_, err))
      return false;
    if (!validation->exists() && !validation->generated_by_dep_loader()) {
      *err = "'" + validation->path() + "' is missing";
      return false;
    }
  }

  std::set<Edge*> seen;
  std::vector<Edge*> dirty_edges;
  CollectDirtyEdges(node, &seen, &dirty_edges);

  // Merge into the pending work list, keeping a single topological order.
  for (Edge* edge : dirty_edges) {
    if (std::find(ready_.begin(), ready_.end(), edge) == ready_.end())
      ready_.push_back(edge);
  }
  std::stable_sort(ready_.begin(), ready_.end(),
                   [](Edge* a, Edge* b) { return a->id_ < b->id_; });

  return true;
}

bool Builder::WriteRspFile(Edge* edge, string* err) {
  string rspfile = edge->GetUnescapedRspfile();
  if (rspfile.empty())
    return true;
  string content = edge->GetBinding("rspfile_content");
  if (!disk_interface_->WriteFile(rspfile, content, false)) {
    *err = "writing rspfile '" + rspfile + "'";
    return false;
  }
  return true;
}

TimeStamp Builder::GetBuildStartTime(string* err) {
  // The build log records a real file's mtime for each command, so that it can
  // be compared against the mtimes of the edge's inputs and outputs; the wall
  // clock is in different units and cannot.  A lock file written once per build
  // supplies that timestamp.
  //
  // It is written exactly once: worker threads run concurrently, and rewriting
  // the same file from every edge would both serialise on the filesystem and
  // leave a descriptor open that children can inherit.
  {
    lock_guard<mutex> lock(bookkeeping_mutex_);
    if (build_start_time_ != -1)
      return build_start_time_;
    disk_interface_->WriteFile(lock_file_path_, ".", /*crlf_on_windows=*/false);
    TimeStamp stamp = disk_interface_->Stat(lock_file_path_, err);
    build_start_time_ = stamp == -1 ? 0 : stamp;
    return build_start_time_;
  }
}

// Sinks for measurements taken in subprocess.cc and plan.cc, which cannot see
// the accumulators above.
void RecordReapMillis(int64_t ms);
void RecordWorkerWait(int64_t find_ms, int64_t wait_ms, int64_t sleep_ms,
                      int64_t polls, int64_t empty_scans);


namespace {

// Optional phase accounting, enabled with SNINJA_TIME_PHASES=1.
//
// The point is to find out where a build actually spends its wall clock before
// optimising anything: the guesses about which part is slow have been wrong
// before, so the breakdown is measured rather than argued.
struct PhaseTimings {
  int64_t spawn_ms = 0;      // CreateProcess
  int64_t run_ms = 0;        // waiting for the child to exit
  int64_t reap_ms = 0;       // joining the output reader after the exit
  int64_t finish_ms = 0;     // FinishCommand (logs, deps, stats)
  int64_t pool_wait_ms = 0;  // blocked waiting for a pool slot
  int edges = 0;
  // Worker-side accounting, for finding out why -j 4 does not reach full
  // concurrency: time spent scanning for work, time spent parked because there
  // was none, and time lost to the fixed poll interval.
  int64_t find_work_ms = 0;
  int64_t wait_work_ms = 0;
  int64_t poll_sleep_ms = 0;
  int64_t polls = 0;
  int64_t empty_scans = 0;
  // Handle count around CreateProcess: with bInheritHandles = TRUE every
  // child duplicates the parent's open handles, so this is the cost of
  // launching while N other commands are in flight.
  int64_t handles_before = 0;
  int64_t handles_after = 0;

  static PhaseTimings& Get() {
    static PhaseTimings t;
    return t;
  }
  static bool Enabled() { return getenv("SNINJA_TIME_PHASES") != nullptr; }
  static void Add(int64_t* slot, int64_t ms) {
    if (Enabled())
      *slot += ms;
  }
  static void Bump(int64_t* slot) {
    if (Enabled())
      ++*slot;
  }
};

}  // namespace

// Sink for the subprocess's post-exit reader-join measurement, which lives in
// subprocess.cc and cannot see the accumulators above.
void RecordReapMillis(int64_t ms) {
  PhaseTimings::Add(&PhaseTimings::Get().reap_ms, ms);
}

void RecordWorkerWait(int64_t find_ms, int64_t wait_ms, int64_t sleep_ms,
                      int64_t polls, int64_t empty_scans) {
  PhaseTimings& t = PhaseTimings::Get();
  PhaseTimings::Add(&t.find_work_ms, find_ms);
  PhaseTimings::Add(&t.wait_work_ms, wait_ms);
  PhaseTimings::Add(&t.poll_sleep_ms, sleep_ms);
  PhaseTimings::Add(&t.polls, polls);
  PhaseTimings::Add(&t.empty_scans, empty_scans);
}

bool Builder::StartEdge(RunningCommand* cmd, Edge* edge, string* err) {
  cmd->edge = edge;
  cmd->start_time = GetTimeMillis();
  cmd->deps_msvc = false;

  if (config_.dry_run || edge->is_phony()) {
    // Nothing to launch: a dry run and a phony edge both finish immediately.
    cmd->subprocess.reset();
    return true;
  }

  // The first real command needs a working compiler environment.  Setting it up
  // costs about a second (it runs vcvars64.bat), so it is done lazily here
  // rather than at startup: a no-op build or a `-t` tool then pays nothing.
  static std::once_flag once;
  std::call_once(once, [] { SetupMsvcEnvironment(); });

  for (Node* out : edge->outputs_) {
    if (!disk_interface_->MakeDirs(out->path()))
      return false;
  }
  edge->command_start_time_ = GetBuildStartTime(err);

  if (!WriteRspFile(edge, err))
    return false;
  if (config_.verbosity == BuildConfig::COMMAND) {
    string command = edge->EvaluateCommand();
    status_->Info("%s", command.c_str());
  }

  cmd->deps_msvc = edge->GetBinding("deps") == "msvc";
  cmd->subprocess = std::make_unique<Subprocess>(edge->use_console(),
                                                cmd->deps_msvc);
  // Every child is created with bInheritHandles = TRUE (it needs our std
  // handles), so CreateProcess duplicates whatever else this process happens to
  // have open.  Track the handle count to see whether that grows with
  // parallelism.
  const int handles_before = GetProcessHandleCount();
  const int64_t before_spawn = GetTimeMillis();
  if (!cmd->subprocess->Start(edge, err))
    return false;
  PhaseTimings::Add(&PhaseTimings::Get().spawn_ms,
                    GetTimeMillis() - before_spawn);
  PhaseTimings::Get().handles_before += handles_before;
  PhaseTimings::Get().handles_after += GetProcessHandleCount();
  PhaseTimings::Get().edges++;

  status_->BuildEdgeStarted(edge, cmd->start_time);
  return true;
}

bool Builder::PollEdge(RunningCommand* cmd, bool* finished, string* err) {
  // A dry run and a phony edge have no process, so they are finished the moment
  // they are started.  Reporting "not finished" here would spin forever.
  if (!cmd->subprocess) {
    *finished = true;
    return true;
  }
  *finished = false;
  int exit_code = 0;
  if (!cmd->subprocess->TryFinish(finished, &exit_code, err))
    return false;
  if (*finished) {
    cmd->exit_code = exit_code;
    if (cmd->observed_exit_time == 0) {
      cmd->observed_exit_time = GetTimeMillis();
      // Time from launch to the child actually exiting.  Compared against the
      // edge's total wall clock it separates "the child took this long" from
      // "we spent this long noticing and reaping it".
      PhaseTimings::Add(&PhaseTimings::Get().run_ms,
                        cmd->observed_exit_time - cmd->start_time);
    }
  }
  return true;
}

bool Builder::FinishEdge(RunningCommand* cmd, string* err) {
  Edge* edge = cmd->edge;
  const int64_t end_time = GetTimeMillis();
  const bool success = cmd->exit_code == 0 && !config_.dry_run &&
                       !edge->is_phony();
  // A dry run and a phony edge are always "successful": nothing ran.
  const bool ok = cmd->exit_code == 0;

  // For msvc deps the command's output was captured, so echo it to the user with
  // the /showIncludes lines stripped out -- even on failure.
  string output;
  const std::string* captured = nullptr;
  if (cmd->subprocess) {
    captured = &cmd->subprocess->output();
    if (cmd->deps_msvc) {
      CLParser parser;
      std::string filtered;
      parser.Parse(*captured, edge->GetBinding("msvc_deps_prefix"), &filtered);
      output = filtered;
    }
  }

  if (!ok) {
    char buf[64];
    snprintf(buf, sizeof(buf), "exit %d: ", cmd->exit_code);
    if (!output.empty())
      output.append("\n");
    output.append(buf);
    output.append(cmd->subprocess ? cmd->subprocess->command() : edge->rule().name());
  }
  if (!output.empty()) {
    fwrite(output.data(), 1, output.size(), stderr);
    fflush(stderr);
  }

  if (!edge->is_phony())
    status_->BuildEdgeFinished(edge, cmd->start_time, end_time, ok, output);

  if (!ok) {
    *err = output;
    return false;
  }

  const int64_t finish_start = GetTimeMillis();
  const bool finished = FinishCommand(edge, (int)cmd->start_time,
                                 (int)end_time, /*success=*/success, err,
                                 captured);
  PhaseTimings::Add(&PhaseTimings::Get().finish_ms, GetTimeMillis() - finish_start);
  return finished;
}
bool Builder::RunEdgeSynchronously(Edge* edge, string* err) {
  RunningCommand cmd;
  if (!StartEdge(&cmd, edge, err))
    return false;
  if (cmd.subprocess) {
    const string* failure = cmd.subprocess->Wait(err);
    cmd.exit_code = failure ? 1 : 0;
    if (failure)
      *err = *failure;
  }
  return FinishEdge(&cmd, err);
}

bool Builder::FinishCommand(Edge* edge, int start_time, int end_time,
                            bool success, string* err,
                            const string* subprocess_output) {
  // The command itself has finished, so the worker is free; everything below
  // touches shared state (the build log, the deps log, State's node table and
  // the nodes' stats) and must not run concurrently with another edge's.
  lock_guard<mutex> lock(bookkeeping_mutex_);

  // Parse deps first: for msvc this filters the /showIncludes lines out of the
  // command output, and extraction failures must fail the build.
  std::vector<Node*> deps_nodes;
  string deps_type = edge->GetBinding("deps");
  if (success && !deps_type.empty()) {
    if (!ExtractDeps(edge, deps_type, &deps_nodes,
                     subprocess_output ? *subprocess_output : string(), err))
      return false;
  }

  if (!success)
    return true;

  // What gets recorded in the build log for this command.  It has to be a
  // timestamp on the same scale as the mtimes of the edge's inputs and outputs,
  // because that is what a later run compares against, and it has to be at
  // least as late as the command's own start -- otherwise a freshly built edge
  // looks stale and gets rebuilt forever.
  //
  // The outputs' own mtimes are both correct and comparable, so use them, and
  // fall back to the build's start time (from the lock file) only when the stat
  // fails.  Recording the build start instead would be cheaper, but it is
  // earlier than everything the build produced, which makes every consumer of
  // the log -- including the reference ninja -- consider the edge out of date.
  TimeStamp record_mtime = 0;
  if (!config_.dry_run) {
    for (Node* out : edge->outputs_) {
      TimeStamp mtime = disk_interface_->Stat(out->path(), err);
      if (mtime == -1)
        return false;
      if (mtime > record_mtime)
        record_mtime = mtime;
    }
  }
  if (record_mtime == 0)
    record_mtime = edge->command_start_time_;

  if (build_log_ && !build_log_path_.empty()) {
    if (!build_log_->RecordCommand(edge, start_time, end_time, record_mtime)) {
      *err = string("writing to build log: ") + strerror(errno);
      return false;
    }
  }

  // Delete any left over response file.
  string rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty())
    disk_interface_->RemoveFile(rspfile);

  if (!deps_type.empty() && !config_.dry_run && deps_log_ &&
      !deps_log_path_.empty()) {
    for (Node* out : edge->outputs_) {
      TimeStamp deps_mtime = disk_interface_->Stat(out->path(), err);
      if (deps_mtime == -1)
        return false;
      if (!deps_log_->RecordDeps(out, deps_mtime, deps_nodes)) {
        *err = string("writing to deps log: ") + strerror(errno);
        return false;
      }
    }
  }

  // Outputs are now up to date.
  for (Node* out : edge->outputs_) {
    if (!config_.dry_run) {
      if (!out->StatIfNecessary(disk_interface_, err))
        return false;
    }
    out->set_dirty(false);
  }
  edge->outputs_ready_ = true;

  return true;
}

bool Builder::ExtractDeps(Edge* edge, const string& deps_type,
                          std::vector<Node*>* deps_nodes,
                          const string& subprocess_output, string* err) {
  deps_nodes->clear();

  if (deps_type == "msvc") {
    CLParser parser;
    std::string filtered;
    parser.Parse(subprocess_output, edge->GetBinding("msvc_deps_prefix"),
                 &filtered);
    for (const std::string& include : parser.includes_) {
      // ~0 assumes all slashes are backslashes, which is fine for MSVC output
      // and matches upstream ninja's behaviour.
      deps_nodes->push_back(state_->GetNode(include, ~0u));
    }
    return true;
  }

  if (deps_type != "gcc" && deps_type != "zero")
    return true;

  string depfile = edge->GetUnescapedDepfile();
  if (depfile.empty()) {
    *err = string("edge with deps=") + deps_type + " but no depfile makes no sense";
    return false;
  }

  // Read depfile content.  Treat a missing depfile as empty.
  string content;
  switch (disk_interface_->ReadFile(depfile, &content, err)) {
    case DiskInterface::Okay:
      break;
    case DiskInterface::NotFound:
      err->clear();
      break;
    case DiskInterface::OtherError:
      return false;
  }
  if (content.empty())
    return true;

  if (deps_type == "gcc") {
    DepfileParser parser;
    if (!parser.Parse(&content, err))
      return false;
    for (std::string_view i : parser.ins_) {
      uint64_t slash_bits;
      size_t len = i.size();
      CanonicalizePath(const_cast<char*>(i.data()), &len, &slash_bits);
      deps_nodes->push_back(
          state_->GetNode(std::string_view(i.data(), len), slash_bits));
    }
  } else {  // "zero"
    const char* ptr = content.c_str();
    const char* const end = &content.back() + 1;
    while (ptr < end) {
      std::string_view piece(ptr);
      ptr += piece.size() + 1;
      uint64_t slash_bits;
      size_t len = piece.size();
      CanonicalizePath(const_cast<char*>(piece.data()), &len, &slash_bits);
      deps_nodes->push_back(
          state_->GetNode(std::string_view(piece.data(), len), slash_bits));
    }
  }

  if (disk_interface_->RemoveFile(depfile) < 0) {
    *err = string("deleting depfile: ") + strerror(errno);
    return false;
  }
  return true;
}


bool Builder::ReportBuildFailure(Edge* edge, const string& message,
                                 string* err_out) {
  status_->Error("%s", message.c_str());
  for (vector<Node*>::const_iterator out = edge->outputs_.begin();
       out != edge->outputs_.end(); ++out) {
    const string& path = (*out)->path();
    if (!path.empty())
      status_->Error("%s", path.c_str());
  }
  *err_out = message;
  return config_.keep_going;
}

bool Builder::Build(string* err) {
  status_->BuildStarted();

  Plan plan;
  plan.set_parallelism(config_.parallelism);
  plan.SetEdges(ready_);
  status_->SetTotalEdges(static_cast<int>(ready_.size()));
  string prepare_err;

  if (!plan.Prepare(&prepare_err)) {
    status_->BuildFinished();
    *err = prepare_err;
    return false;
  }

  const bool success = plan.Run(this, err);

  if (PhaseTimings::Enabled()) {
    const PhaseTimings& t = PhaseTimings::Get();
    fprintf(stderr,
            "[phases] edges=%d spawn=%lldms child_run=%lldms reap=%lldms "
            "finish=%lldms (serialised) handles=%lld->%lld (avg)\n",
            t.edges, (long long)t.spawn_ms, (long long)t.run_ms,
            (long long)t.reap_ms, (long long)t.finish_ms,
            t.edges ? t.handles_before / t.edges : 0,
            t.edges ? t.handles_after / t.edges : 0);
    fprintf(stderr,
            "[phases] worker: find=%lldms park=%lldms sleep=%lldms "
            "polls=%lld empty_scans=%lld (summed over all workers)\n",
            (long long)t.find_work_ms, (long long)t.wait_work_ms,
            (long long)t.poll_sleep_ms, (long long)t.polls,
            (long long)t.empty_scans);
  }

  status_->BuildFinished();

  return success;
}
