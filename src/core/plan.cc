#include "plan.h"

#include <deque>

#include <algorithm>
#include <set>

#include "build.h"
#include "subprocess.h"
#include <windows.h>
#include "steal.h"
#include "util.h"
#include "interrupt.h"
#include "job.h"
#include "timeline.h"

using namespace std;
using sninja::StealDeque;

Plan::Plan() {
  interrupt_event_ = CreateEventA(nullptr, /*manualReset=*/TRUE,
                                  /*initialState=*/FALSE, nullptr);
}

Plan::~Plan() {
  Cleanup();
  if (interrupt_event_)
    CloseHandle(static_cast<HANDLE>(interrupt_event_));
  for (auto& deque : ready_)
    deque.reset();
}

void Plan::Cleanup() {
  bool already_interrupted;
  {
    lock_guard<mutex> lock(mutex_);
    already_interrupted = interrupted_;
    interrupted_ = true;
  }
  // Every waiter must be woken even when we were already interrupted.  Returning
  // early here is what used to strand the spawner thread: it parks on
  // launch_available_, and nothing else ever signals it, so the process
  // finished the whole build and then hung on join.
  if (already_interrupted) {
    SetEvent(static_cast<HANDLE>(interrupt_event_));
    work_available_.notify_all();
    pool_available_.notify_all();
    launch_available_.notify_all();
    return;
  }
  // Wake any worker blocked in WaitForMultipleObjects on its children, and any
  // blocked in EnqueueLaunch.
  if (interrupt_event_)
    SetEvent(static_cast<HANDLE>(interrupt_event_));
  work_available_.notify_all();
  pool_available_.notify_all();
  launch_available_.notify_all();

  // Kill whatever is still running.  The workers do this themselves for the
  // commands they own, but a console interrupt arrives on a thread of its own
  // and Cleanup() is what the callers reach first; doing it here means the
  // children cannot outlive the build by more than one scheduling quantum.
  if (JobObjectEnabled())
    KillOnCloseJob::Global()->KillAll();
}


void Plan::BuildDependencyGraph() {
  pending_.clear();
  dependents_.clear();

  // Map each output node to the edge that produces it, so a dependent can find
  // the prerequisites that are actually being run.
  unordered_map<Node*, Edge*> producer;
  for (Edge* edge : edges_) {
    for (Node* out : edge->outputs_)
      producer[out] = edge;
  }

  for (Edge* edge : edges_) {
    // Count each prerequisite once, even if several of its outputs are inputs
    // of this edge.
    set<Edge*> prerequisites;
    for (Node* in : edge->inputs_) {
      auto it = producer.find(in);
      if (it != producer.end() && it->second != edge)
        prerequisites.insert(it->second);
    }
    pending_[edge] = static_cast<int>(prerequisites.size());
    for (Edge* prerequisite : prerequisites)
      dependents_[prerequisite].push_back(edge);
  }
}

bool Plan::Prepare(string* err) {
  BuildDependencyGraph();

  ready_.clear();
  ready_.reserve(parallelism_);
  for (int i = 0; i < parallelism_; ++i)
    ready_.push_back(make_unique<StealDeque>());

  int initial = 0;
  for (Edge* edge : edges_) {
    if (pending_[edge] == 0) {
      EnqueueReady(edge);
      ++initial;
    }
  }
  unfinished_.store(static_cast<int>(edges_.size()), memory_order_release);

  if (getenv("SNINJA_TRACE_PLAN")) {
    fprintf(stderr, "[plan] %d edges, parallelism=%d, initially ready=%d\n",
            static_cast<int>(edges_.size()), parallelism_, initial);
    for (Edge* edge : edges_) {
      fprintf(stderr, "[plan]   %-50s pending=%d outs=%d%s\n",
              edge->rule().name().c_str(), pending_[edge],
              static_cast<int>(edge->outputs_.size()),
              edge->outputs_.empty() ? "" : edge->outputs_[0]->path().c_str());
    }
  }


  if (initial == 0 && !edges_.empty()) {
    *err = "dependency cycle detected";
    return false;
  }
  return true;
}

void Plan::EnqueueReady(Edge* edge) {
  // The push and the wake-up must be ordered against WaitForWork's predicate
  // check, so both happen under |mutex_|.  Pushing is one atomic store per
  // edge, so the contention is irrelevant.
  lock_guard<mutex> lock(mutex_);

  // Round-robin across the workers' rings so that thieves have something to
  // take from.
  static atomic<unsigned> next_worker{0};
  unsigned base = next_worker.fetch_add(1, memory_order_relaxed);
  for (unsigned i = 0; i < static_cast<unsigned>(parallelism_); ++i) {
    if (ready_[(base + i) % parallelism_]->Push(edge)) {
      work_available_.notify_all();
      return;
    }
  }
  // Every ring is full: fall back to the shared queue.
  ready_shared_.push_back(edge);
  work_available_.notify_all();
}

Edge* Plan::TakeSharedWork() {
  lock_guard<mutex> lock(mutex_);
  if (ready_shared_.empty())
    return nullptr;
  Edge* edge = ready_shared_.front();
  ready_shared_.pop_front();
  return edge;
}


bool Plan::AcquirePoolSlot(Pool* pool, Edge* edge) {
  if (!pool || !pool->ShouldDelayEdge())
    return true;
  unique_lock<mutex> lock(mutex_);
  while (pool->current_use() + edge->weight() > pool->depth()) {
    if (interrupted_)
      return false;
    pool_available_.wait(lock);
  }
  pool->EdgeScheduled(*edge);
  return true;
}

void Plan::ReleasePoolSlot(Pool* pool, Edge* edge) {
  if (!pool || !pool->ShouldDelayEdge())
    return;
  {
    lock_guard<mutex> lock(mutex_);
    pool->EdgeFinished(*edge);
  }
  pool_available_.notify_all();
}

bool Plan::HasReadyWorkLocked() const {
  if (!ready_shared_.empty())
    return true;
  for (const auto& deque : ready_) {
    if (deque && !deque->empty())
      return true;
  }
  return false;
}

bool Plan::AnyWork() const {
  lock_guard<mutex> lock(mutex_);
  return interrupted_ || HasReadyWorkLocked() ||
         unfinished_.load(memory_order_acquire) == 0;
}

void Plan::WaitForWork() {
  unique_lock<mutex> lock(mutex_);
  // The predicate must cover the rings as well as the shared queue: a worker
  // that parks must not miss an edge pushed to a ring it does not own.
  work_available_.wait(lock, [this] {
    return interrupted_ || HasReadyWorkLocked() ||
           unfinished_.load(memory_order_acquire) == 0;
  });
}


Edge* Plan::TryFindWork(int worker_index) {
  if (interrupted_)
    return nullptr;
  StealDeque* mine = ready_[worker_index].get();

  // 1. Take from the back of our own ring.  LIFO: the edges most recently
  //    unblocked are the ones whose inputs are already on disk.
  if (Edge* edge = static_cast<Edge*>(mine->Pop()))
    return edge;

  // 2. Steal from another worker.  FIFO: the oldest edges, which have been
  //    waiting longest.
  for (int i = 1; i < parallelism_; ++i) {
    int victim = (worker_index + i) % parallelism_;
    if (Edge* edge = static_cast<Edge*>(ready_[victim]->Steal()))
      return edge;
  }

  // 3. Whatever did not fit in any ring.
  return TakeSharedWork();
}

Edge* Plan::FindWork(int worker_index) {
  for (;;) {
    if (Edge* edge = TryFindWork(worker_index))
      return edge;
    if (interrupted_)
      return nullptr;
    // Is everything done?
    if (unfinished_.load(memory_order_acquire) == 0)
      return nullptr;
    // Nothing ready right now: sleep until an edge becomes ready or the build
    // ends.  Deliberately called with no run slot held, so parking costs
    // nothing -- a slot parked on an idle lookup can starve the workers that
    // do have work.
    WaitForWork();
  }
}


void Plan::EdgeFinished(Edge* edge, EdgeResult result, const string& error) {
  vector<Edge*> newly_ready;
  {
    lock_guard<mutex> lock(mutex_);
    if (result == EdgeResult::kFailed && failures_++ == 0)
      first_error_ = error;
  }

  auto it = dependents_.find(edge);
  if (it != dependents_.end()) {
    for (Edge* dependent : it->second) {
      if (--pending_[dependent] == 0)
        newly_ready.push_back(dependent);
    }
  }

  unfinished_.fetch_sub(1, memory_order_release);
  work_available_.notify_all();
  pool_available_.notify_all();

  for (Edge* ready : newly_ready)
    EnqueueReady(ready);
}

bool Plan::AcquireRunSlot() {
  int current = running_.load(std::memory_order_relaxed);
  while (current < parallelism_) {
    if (running_.compare_exchange_weak(current, current + 1,
                                       std::memory_order_acq_rel,
                                       std::memory_order_relaxed))
      return true;
    // |current| was refreshed by the failed exchange.
  }
  return false;
}

void Plan::ReleaseRunSlot() {
  running_.fetch_sub(1, std::memory_order_acq_rel);
}

bool Plan::SpawnerEnabled() const {
  // The spawner moves CreateProcess (~4.3 ms per edge here) off the worker threads.
  // Whether that is a win depends entirely on how many workers there are to share
  // the cost, because the spawner is a single thread and becomes a queue of its own
  // once it cannot keep up.  Measured on a 120-unit project, best of 3:
  //
  //     -j   spawner   in-worker   ninja
  //      2    3.03s      3.33s    3.10s     spawner wins by 9%
  //      4    1.68s      2.03s    1.79s     spawner wins by 6%
  //      8    1.22s      1.28s    1.30s     about even
  //     16    0.95s      0.90s    1.07s     spawner loses: overhead already amortised
  //     24    1.11s      0.76s    1.03s     spawner loses badly: 123 edges x 4.3 ms
  //                                               all queued behind one thread
  //
  // So it is enabled only at low -j, where each worker spends ~7% of its wall clock
  // inside CreateProcess, and left off at high -j, where that fraction is small
  // enough that a serial launch thread only adds a bottleneck.
  //
  // SNINJA_SPAWNER overrides the decision, and takes an explicit value rather than
  // being a mere presence flag: SNINJA_SPAWNER=0 has to mean off, otherwise there
  // is no way to measure the in-worker path on a build whose -j would otherwise
  // pick the spawner.
  if (const char* setting = getenv("SNINJA_SPAWNER")) {
    if (!*setting)
      return parallelism_ <= kSpawnerMaxJobs;
    // Compare case-insensitively without depending on <strings.h>, which is not
    // available on Windows.
    std::string value(setting);
    for (char& c : value)
      c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return !(value == "0" || value == "false" || value == "no" ||
             value == "off");
  }

  return parallelism_ <= kSpawnerMaxJobs;
}

bool Plan::EnqueueLaunch(RunningCommand* cmd, Edge* edge) {
  if (!SpawnerEnabled())
    return false;
  Launch* job = new Launch();
  job->cmd = cmd;
  job->edge = edge;
  // A manual-reset event, and this matters: the same handle is waited on by
  // WaitForMultipleObjects and then polled with a zero timeout when the command
  // is harvested.  An auto-reset event would have its signal consumed by the
  // first waiter, so the poll would report "not launched yet" and the worker
  // would wait for a signal that never comes again -- a hang.  The worker is the
  // only holder of this handle and closes it after harvesting, so the signal
  // staying latched is harmless.
  job->done = CreateEventA(nullptr, /*manualReset=*/TRUE,
                           /*initialState=*/FALSE, nullptr);
  if (!job->done) {
    delete job;
    return false;
  }
  // Hand the event to the command, because that is what the worker harvests on and
  // what WaitForAnyChild puts in its wait set.  Without this the command's
  // launch_event stays null: the harvest never succeeds, so `pending` never
  // drains, while the reap loop -- which skips commands on launch_event being set
  // -- polls commands that have no process, gets "finished" immediately, and
  // reports every edge as succeeded without a single command having run.
  cmd->launch_event = job->done;
  {
    lock_guard<mutex> lock(mutex_);
    if (interrupted_) {
      CloseHandle(static_cast<HANDLE>(job->done));
      delete job;
      return false;
    }
    launch_queue_.push_back(job);
  }
  launch_available_.notify_one();
  return true;
}

void Plan::SpawnerMain(Builder* builder) {
  for (;;) {
    Launch* job = nullptr;
    {
      unique_lock<mutex> lock(mutex_);
      launch_available_.wait(lock,
                             [this] { return interrupted_ ||
                                      !launch_queue_.empty(); });
      if (launch_queue_.empty()) {
        if (interrupted_)
          break;
        continue;
      }
      job = launch_queue_.front();
      launch_queue_.erase(launch_queue_.begin());
      // Read the cancel flag while still holding the lock: the drain sets it
      // under this same lock before it starts waiting, so a teardown can never
      // be missed here.
      job->cancelled = job->cmd->launch_cancelled;
    }
    // Outside the lock: StartEdge does I/O (MakeDirs, response files,
    // CreateProcess) and must not serialise the workers' bookkeeping.
    string err;
    if (job->cancelled) {
      // The build is tearing down.  Do not create the process: a command
      // launched now would have nobody to reap it.
      job->cmd->launch_failed = true;
      err = "cancelled before launch";
    } else {
      builder->StartEdge(job->cmd, job->edge, &err);
    }
    job->err = std::move(err);
    SetEvent(static_cast<HANDLE>(job->done));
  }

  // Anything still queued when we are shutting down will never be signalled, so
  // wake the workers that are waiting on it.
  for (Launch* job : launch_queue_) {
    if (job->done)
      SetEvent(static_cast<HANDLE>(job->done));
  }
  launch_queue_.clear();
}

void Plan::WaitForAnyChild(std::vector<RunningCommand*>& running,
                           std::vector<RunningCommand*>& pending,
                           bool measuring, int64_t* sleep_ms, int64_t* polls) {
  // Ctrl-C (or a console close) is turned into a build stop by the handler in
  // interrupt.cc, which signals this event.  Children are created with
  // CREATE_NEW_PROCESS_GROUP, so a console Ctrl-C no longer reaches them
  // directly -- without this the build would carry on regardless.
  if (InterruptRequested()) {
    Cleanup();
    return;
  }

  // WaitForMultipleObjects accepts at most MAXIMUM_WAIT_OBJECTS handles.
  constexpr size_t kMaxWait = 60;
  HANDLE handles[kMaxWait + 1];
  size_t n = 0;
  for (RunningCommand* cmd : running) {
    if (n == kMaxWait)
      break;
    void* h = cmd->subprocess ? cmd->subprocess->handle() : nullptr;
    if (h)
      handles[n++] = static_cast<HANDLE>(h);
  }
  // A pending launch is not yet a process, but it is something to wake up for.
  for (RunningCommand* cmd : pending) {
    if (n == kMaxWait)
      break;
    if (cmd->launch_event)
      handles[n++] = static_cast<HANDLE>(cmd->launch_event);
  }
  if (n == 0) {
    Sleep(1);
    return;
  }

  // Last slots: the interrupt event so Cleanup() can break the wait, and the
  // console-interrupt event so a Ctrl-C can break it too.
  HANDLE interrupt = static_cast<HANDLE>(interrupt_event_);
  if (interrupt)
    handles[n++] = interrupt;
  HANDLE console_interrupt = ConsoleInterruptEvent();
  if (console_interrupt && n < kMaxWait + 1)
    handles[n++] = console_interrupt;

  const int64_t start = measuring ? GetTickCount64() : 0;
  // A timeout rather than INFINITE, so a worker can never wedge permanently if
  // a signal is missed.
  WaitForMultipleObjects(static_cast<DWORD>(n), handles, /*waitAll=*/FALSE,
                         /*timeout=*/100);
  if (measuring) {
    *sleep_ms += GetTickCount64() - start;
    ++*polls;
  }
}

bool ChildKillEnabled() {
  // The second half of the interrupt cleanup.  SNINJA_NO_CHILD_KILL turns off the
  // per-child TerminateProcess in the drain, so the interrupt tests can observe
  // what the job object alone does -- and confirm the test is load-bearing by
  // watching it fail when both mechanisms are disabled.
  return getenv("SNINJA_NO_CHILD_KILL") == nullptr;
}

void Plan::WorkerMain(int worker_index, Builder* builder) {
  // Ownership of every command this worker has created, in a container that is
  // only ever appended to.  |running| and |pending| hold pointers into it, and
  // reaping removes entries from those lists -- so the pointed-to objects must
  // not move or be destroyed while anything still refers to them.  (Holding the
  // RunningCommand by value in the list being pruned is what made this
  // use-after-free: erasing one invalidated the references the pending list was
  // still holding.)
  std::vector<std::unique_ptr<RunningCommand>> owned;
  // Launched (or being launched) and not yet reaped.  Keeping several in flight
  // is the whole point: a worker does not sit in WaitForSingleObject while a
  // compiler runs, it goes and starts the next one.
  std::vector<RunningCommand*> running;
  // Handed to the spawner but not yet harvested: no process yet, so they must be
  // neither polled nor waited on until their launch event fires.
  std::vector<RunningCommand*> pending;
  string err;

  // Worker-side accounting, so that a concurrency shortfall can be attributed to
  // a specific place (looking for work, parked with nothing to do, or the poll
  // interval) instead of guessed at.
  extern void RecordWorkerWait(int64_t find_ms, int64_t wait_ms,
                               int64_t sleep_ms, int64_t polls,
                               int64_t empty_scans);
  const bool measuring = getenv("SNINJA_TIME_PHASES") != nullptr;
  int64_t find_ms = 0, wait_ms = 0, sleep_ms = 0, polls = 0, empty_scans = 0;
  const int64_t wall_start = GetTickCount64();

  for (;;) {
    if (interrupted_)
      break;

    // Top up: launch as many commands as the global budget allows.
    while (AcquireRunSlot()) {
      // Non-blocking: a slot must never be parked on a worker with nothing to do.
      const int64_t find_start = measuring ? GetTickCount64() : 0;
      Edge* edge = TryFindWork(worker_index);
      if (measuring) {
        find_ms += GetTickCount64() - find_start;
        if (!edge)
          ++empty_scans;
      }
      if (!edge) {
        ReleaseRunSlot();
        break;
      }

      Pool* pool = edge->pool();
      if (!AcquirePoolSlot(pool, edge)) {
        ReleaseRunSlot();
        interrupted_ = true;
        break;
      }

      owned.emplace_back(new RunningCommand());
      RunningCommand* cmd = owned.back().get();
      cmd->edge = edge;
      cmd->pool = pool;
      running.push_back(cmd);

      Timeline::Record(Timeline::Kind::kEdgeStart, worker_index, edge);
      if (EnqueueLaunch(cmd, edge)) {
        // The spawner thread does the CreateProcess; this worker stays free to
        // reap whatever it already has running.  The event it will be signalled
        // on is what the reap wait below watches.
        pending.push_back(cmd);
      } else {
        // Launched on the worker thread.  This is the default path; the spawner
        // is opt-in and off.  StartEdge does the CreateProcess, so this is the
        // point at which the child actually exists.
        if (!builder->StartEdge(cmd, edge, &err)) {
          ReleasePoolSlot(pool, edge);
          ReleaseRunSlot();
          running.pop_back();
          EdgeFinished(edge, EdgeResult::kFailed, err);
          if (!builder->config().keep_going) {
            Cleanup();
            goto drain;
          }
          break;
        }
      }
    }

    // Harvest launches the spawner has finished.  Until this point the command
    // has no process, so it must not be polled or waited on.
    for (size_t i = 0; i < pending.size();) {
      RunningCommand* cmd = pending[i];
      if (WaitForSingleObject(static_cast<HANDLE>(cmd->launch_event), 0) !=
          WAIT_OBJECT_0) {
        ++i;
        continue;
      }
      CloseHandle(static_cast<HANDLE>(cmd->launch_event));
      cmd->launch_event = nullptr;
      Edge* edge = cmd->edge;
      // Both launch paths converge here, so this is the only place that can say
      // truthfully that the child now exists.
      Timeline::Record(Timeline::Kind::kLaunched, worker_index, edge);
      pending[i] = pending.back();
      pending.pop_back();

      if (cmd->launch_failed) {
        // StartEdge failed on the spawner thread.  Report it exactly as the
        // inline launch path would have, and take it out of |running|: there is
        // no process to reap.
        ReleasePoolSlot(cmd->pool, edge);
        ReleaseRunSlot();
        for (size_t j = 0; j < running.size(); ++j) {
          if (running[j] == cmd) {
            running.erase(running.begin() + j);
            break;
          }
        }
        EdgeFinished(edge, EdgeResult::kFailed, cmd->launch_err);
        if (!builder->config().keep_going) {
          Cleanup();
          goto drain;
        }
      }
    }

    if (running.empty() && pending.empty()) {
      // Nothing of ours is in flight.  Either the build is over, or we have to
      // wait for somebody else to finish something.
      if (unfinished_.load(memory_order_acquire) == 0)
        break;
      const int64_t wait_start = measuring ? GetTickCount64() : 0;
      WaitForWork();
      if (measuring)
        wait_ms += GetTickCount64() - wait_start;
      continue;
    }


    // Reap whatever has finished.  A poll that finds nothing means we should
    // sleep rather than spin, since the processes will not change state
    // without help from the OS.
    bool reaped_any = false;
    for (size_t i = 0; i < running.size();) {
      RunningCommand* cmd = running[i];
      // A command still waiting to be launched has no process.  Polling it
      // would hit the "dry run / phony finishes immediately" path and report
      // success for a command that never ran.
      if (cmd->launch_event) {
        ++i;
        continue;
      }
      bool finished = false;
      if (!builder->PollEdge(cmd, &finished, &err)) {
        // Could not even ask; treat the command as failed.
        finished = true;
        cmd->exit_code = 1;
      }
      if (!finished) {
        ++i;
        continue;
      }
      Edge* edge = cmd->edge;
      Pool* pool = cmd->pool;
      string edge_err;
      // Recorded separately from kEdgeFinish because the gap between them is the
      // edge's bookkeeping cost: log write, deps capture, stats.
      Timeline::Record(Timeline::Kind::kExited, worker_index, edge);
      const bool ok = builder->FinishEdge(cmd, &edge_err);
      Timeline::Record(Timeline::Kind::kEdgeFinish, worker_index, edge);
      ReleasePoolSlot(pool, edge);
      ReleaseRunSlot();
      running.erase(running.begin() + i);
      EdgeFinished(edge, ok ? EdgeResult::kSucceeded : EdgeResult::kFailed,
                   edge_err);
      reaped_any = true;
      if (!ok && !builder->config().keep_going) {
        Cleanup();
        goto drain;
      }
    }

    if (!reaped_any) {
      // Nothing finished and (because the loop above could not start anything)
      // there is nothing to start.  Polling on a timer costs both CPU and, more
      // importantly, up to a full interval of latency before a just-exited
      // command is noticed -- which shows up directly as missing concurrency.
      // Block in the kernel on the children instead, so the wait ends the
      // instant one of them exits.  The interrupt event keeps Ctrl-C responsive,
      // and the bounded timeout keeps a lost signal from wedging a worker.
      Timeline::Record(Timeline::Kind::kWaitEnter, worker_index, nullptr);
      WaitForAnyChild(running, pending, measuring, &sleep_ms, &polls);
      Timeline::Record(Timeline::Kind::kWaitExit, worker_index, nullptr);
    }
  }

  if (measuring) {
    const int64_t elapsed = GetTickCount64() - wall_start;
    // Attribute whatever is left to reaping, so the numbers add up to the time
    // this worker was alive.
    (void)elapsed;
    RecordWorkerWait(find_ms, wait_ms, sleep_ms, polls, empty_scans);
  }

drain:
  // A command that has been handed to the spawner but not yet harvested has no
  // process, but the spawner may be about to create one.  Cancel those and wait
  // for the outcome, so that no child is created after this point and none is
  // left running.
  for (RunningCommand* cmd : running) {
    if (!cmd->launch_event)
      continue;
    {
      lock_guard<mutex> lock(mutex_);
      cmd->launch_cancelled = true;
    }
    // The spawner always signals, including on its way out, so this cannot hang
    // on a job it has already dropped.
    WaitForSingleObject(static_cast<HANDLE>(cmd->launch_event), INFINITE);
    CloseHandle(static_cast<HANDLE>(cmd->launch_event));
    cmd->launch_event = nullptr;
  }

  // Reap whatever is still in flight so that no child outlives the build, and
  // so the workers' pool slots come back.
  //
  // An interrupted build is the case where a command has not been given a chance
  // to finish cleanly, so the child is killed first: waiting on a compiler that
  // will run to completion anyway would make Ctrl-C take as long as the build.
  const bool interrupted_build = InterruptRequested() && ChildKillEnabled();
  for (RunningCommand* cmd : running) {
    if (cmd->subprocess) {
      if (interrupted_build)
        cmd->subprocess->Terminate();
      string wait_err;
      const string* failure = cmd->subprocess->Wait(&wait_err);
      cmd->exit_code = failure ? 1 : 0;
      if (failure)
        wait_err = *failure;
      string edge_err;
      builder->FinishEdge(cmd, &edge_err);
    }
    ReleasePoolSlot(cmd->pool, cmd->edge);
    ReleaseRunSlot();
  }
  running.clear();
}


  bool Plan::Run(Builder* builder, string* err) {
    if (edges_.empty())
      return true;

    // Take responsibility for Ctrl-C before starting anything: children are
    // created in their own process groups, so they will not be told about it.
    InstallConsoleInterruptHandler();
    if (InterruptRequested()) {
      *err = "interrupted";
      return false;
    }


  // The sequential path avoids all the cross-thread bookkeeping: walk the
  // edges in dependency order on this thread.
  if (parallelism_ <= 1) {
    vector<Edge*> order;
    for (Edge* edge : edges_) {
      if (pending_[edge] == 0)
        order.push_back(edge);
    }
    bool success = true;
    for (size_t i = 0; i < order.size(); ++i) {
      Edge* edge = order[i];
      string edge_err;
      if (!builder->RunEdgeSynchronously(edge, &edge_err)) {
        if (success)
          *err = edge_err;
        success = false;
        if (!builder->config().keep_going)
          break;
      }
      auto it = dependents_.find(edge);
      if (it != dependents_.end()) {
        for (Edge* dependent : it->second) {
          if (--pending_[dependent] == 0)
            order.push_back(dependent);
        }
      }
    }
    return success;
  }

  // The spawner runs CreateProcess off the workers' threads.  It is started
  // before them, and stopped after they have all joined, so no worker can be
  // left waiting on a launch that nobody will perform.
  thread spawner;
  if (SpawnerEnabled())
    spawner = thread([this, builder] { SpawnerMain(builder); });

  for (int i = 1; i < parallelism_; ++i)
    threads_.emplace_back([this, i, builder] { WorkerMain(i, builder); });
  WorkerMain(0, builder);
  for (thread& t : threads_) {
    if (t.joinable())
      t.join();
  }
  threads_.clear();

  if (spawner.joinable()) {
    Cleanup();
    spawner.join();
  }

  // Write the timeline out before returning, so a build that fails or is killed
  // partway through still leaves something to look at.
  Timeline::Flush();


  if (failures_ > 0) {
    if (!first_error_.empty())
      *err = first_error_;
    return false;
  }
  return true;
}
