#ifndef SNINJA_PLAN_H_
#define SNINJA_PLAN_H_

#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "build.h"
#include "graph.h"
#include "state.h"
#include "steal.h"

struct Builder;

/// Runs the dirty edges with a bounded amount of parallelism.
///
/// Edges become ready as their prerequisites finish; ready edges are handed to
/// workers through per-worker rings that workers also steal from, so a worker
/// that runs out of work picks up another worker's backlog instead of idling.
/// Pools cap how much of the build may run at once (the console pool is
/// reserved for edges that talk to the user).
class Plan {
 public:
  Plan();
  ~Plan();

  int parallelism() const { return parallelism_; }
  void set_parallelism(int parallelism) { parallelism_ = parallelism; }

  /// Set the edges that need to run.
  void SetEdges(std::vector<Edge*> edges) { edges_ = std::move(edges); }

  /// Prepare |edges_|: work out when each edge becomes ready and seed the
  /// queues with the ones that are ready now.
  bool Prepare(std::string* err);

  /// Run every prepared edge.  Returns false if any command failed.
  bool Run(Builder* builder, std::string* err);

  /// Ask the workers to stop.
  void Cleanup();

 private:
  /// How an edge finished.
  enum class EdgeResult { kSucceeded, kFailed };

  /// Build the prerequisite/dependent bookkeeping from |edges_|.
  void BuildDependencyGraph();

  /// Pick the next edge for |worker_index|, blocking until one is available.
  /// Returns nullptr once the build is finished or was interrupted.
  Edge* FindWork(int worker_index);

  /// Like FindWork, but returns nullptr immediately when there is nothing ready.
  ///
  /// This must be used when the caller holds a run slot: blocking here would
  /// park a slot on a worker that has no work, and with one slot per worker that
  /// can leave every slot held by an idle lookup while commands are still
  /// queued behind them.
  Edge* TryFindWork(int worker_index);


  /// Block until |pool| has room for |edge|.
  bool AcquirePoolSlot(Pool* pool, Edge* edge);
  void ReleasePoolSlot(Pool* pool, Edge* edge);

  /// Hand |edge| to the workers.
  void EnqueueReady(Edge* edge);

  /// Take an edge from the shared overflow queue, if any.
  Edge* TakeSharedWork();

  /// Called by a worker once |edge| has finished; unblocks its dependents.
  void EdgeFinished(Edge* edge, EdgeResult result, const std::string& error);

  /// A worker thread's main loop: keep as many commands in flight as the budget
  /// allows, reaping whatever has finished, and take on more work as it appears.
  void WorkerMain(int worker_index, Builder* builder);

  /// Block until there may be work to do.
  void WaitForWork();

  /// Block until one of |running|'s children exits, or one of |pending|'s
  /// launches completes, or the build is interrupted.  Replaces polling on a
  /// timer, which cost both CPU and up to a full poll interval of latency per
  /// command.
  void WaitForAnyChild(std::vector<RunningCommand*>& running,
                       std::vector<RunningCommand*>& pending, bool measuring,
                       int64_t* sleep_ms, int64_t* polls);

  /// One unit of work for the spawner thread: run StartEdge (the expensive
  /// CreateProcess and everything it needs) off the worker thread.
  struct Launch {
    RunningCommand* cmd;
    Edge* edge;
    /// Signalled once cmd->subprocess / cmd->launch_failed are set.
    void* done = nullptr;
    std::string err;
    /// Snapshotted from cmd->launch_cancelled while the plan's mutex was held.
    bool cancelled = false;
  };

  /// The spawner thread's loop: drains the launch queue, so a worker's thread is
  /// not occupied by CreateProcess.  CreateProcess costs ~4.3 ms per edge here,
  /// which at low -j was ~7% of a worker's wall clock and put a hard ceiling on
  /// achievable concurrency.
  void SpawnerMain(Builder* builder);

  /// True when CreateProcess should be done on the spawner thread rather than on
  /// the worker.  Depends on the parallelism: the spawner is one thread, so it is
  /// only a win while there are few enough workers that each of them spends real
  /// time inside CreateProcess.  SNINJA_SPAWNER=1 overrides the decision.
  bool SpawnerEnabled() const;

  /// The -j at or below which the spawner is used automatically.  See the
  /// measurements in SpawnerEnabled().
  static constexpr int kSpawnerMaxJobs = 8;

  /// Hand a prepared launch to the spawner.  Returns false if the spawner is
  /// disabled or shutting down, in which case the caller must launch inline.
  bool EnqueueLaunch(RunningCommand* cmd, Edge* edge);



  /// True if some edge is ready to run but has not been taken yet, or the build
  /// is over / was interrupted.  For use while |mutex_| is held.
  bool HasReadyWorkLocked() const;
  /// AnyWork(), for use without |mutex_|.
  bool AnyWork() const;

  /// Try to reserve one of the |parallelism_| concurrent command slots.
  bool AcquireRunSlot();
  void ReleaseRunSlot();

  int parallelism_ = 1;
  std::vector<Edge*> edges_;

  /// Number of prerequisites each edge is still waiting for.
  std::unordered_map<Edge*, int> pending_;
  /// For each edge that still has to run, the edges waiting on it.
  std::unordered_map<Edge*, std::vector<Edge*>> dependents_;

  /// Ready edges, one ring per worker.
  std::vector<std::unique_ptr<sninja::StealDeque>> ready_;
  /// Ready edges that did not fit in any ring.  Guarded by |mutex_|.
  std::deque<Edge*> ready_shared_;

  /// Edges scheduled but not yet finished.  Reaching zero ends the build.
  std::atomic<int> unfinished_{0};
  /// Commands currently running across all workers; bounded by |parallelism_|.
  std::atomic<int> running_{0};
  int failures_ = 0;
  std::string first_error_;
  bool interrupted_ = false;

  mutable std::mutex mutex_;
  std::condition_variable work_available_;
  std::condition_variable pool_available_;

  /// Signalled when the build is interrupted, so that a worker blocked in
  /// WaitForMultipleObjects on its children wakes up instead of waiting for a
  /// child that is never going to finish on its own.
  void* interrupt_event_ = nullptr;

  /// Pending launches handed to the spawner thread, and the signal that there
  /// is more.
  std::vector<Launch*> launch_queue_;
  std::condition_variable launch_available_;

  std::vector<std::thread> threads_;

};

#endif  // SNINJA_PLAN_H_
