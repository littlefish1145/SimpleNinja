#include "timeline.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "graph.h"

namespace {

// Output path, or "<none>".  Only used when writing the CSV, so it does not
// belong on the recording path.
std::string EdgeLabel(const Edge* edge) {
  if (!edge || edge->outputs_.empty() || !edge->outputs_[0])
    return "<none>";
  return edge->outputs_[0]->path();
}

struct Registry {
  std::mutex mutex;
  // edge id -> label.  Written when an edge is first seen; read when flushing.
  // Edge* -> label.  The pointer is only used as an identity key within one run.
  std::unordered_map<const Edge*, std::string> labels;
};

Registry* g_registry = nullptr;
int64_t g_freq = 0;
int64_t g_first = 0;
std::atomic<bool> g_flushed{false};

// The published file is written whole, through a temporary and an atomic rename,
// rather than appended to.  Appending would be simpler, but a snapshot can race
// with itself: two flushes could interleave, and events written by a later flush
// would sort before ones written by an earlier one, breaking the "globally sorted
// by t" assumption the analyser relies on.  Writing a fresh temporary each time
// and renaming it into place means a reader -- including one reading after the
// process was killed -- always sees a complete, self-consistent snapshot.
std::string g_out_path;
std::atomic<uint64_t> g_snapshot{0};

// Set once a flush is in flight, so a snapshot taken mid-write is not renamed
// over a newer one.
std::mutex g_flush_mutex;

// Periodic flush.  Off unless SNINJA_TIMELINE_FLUSH_MS names an interval, because
// the whole point of the default is that a measurement run pays nothing.
int64_t g_flush_interval_ms = 0;
int64_t g_last_flush_ms = 0;

// One buffer per worker, so recording does not contend on a lock.  Sized on
// first use; a worker index beyond the initial size falls back to a shared,
// mutex-protected buffer.
constexpr int kInitialWorkers = 64;
constexpr size_t kMaxEvents = 1u << 20;  // per worker, ~21 MB; a 1M-edge build
                                          // would truncate rather than exhaust
struct Buffer {
  std::vector<Timeline::Event> events;
  bool truncated = false;
};

Buffer g_buffers[kInitialWorkers];
std::mutex g_fallback_mutex;
Buffer g_fallback;

// Serialise every buffer into one time-ordered CSV at |path|, then replace the
// published file atomically.  Returns false if the file could not be produced, in
// which case the previously published snapshot is left untouched.
bool WriteSnapshot(const std::string& path) {
  // A per-flush temporary.  The name includes the snapshot number and the pid so
  // two concurrent builds writing the same target cannot collide, and so a crash
  // leaves an obviously-orphaned file rather than a half-written target.
  char suffix[64];
  snprintf(suffix, sizeof(suffix), ".tmp.%lu.%llu",
           static_cast<unsigned long>(GetCurrentProcessId()),
           static_cast<unsigned long long>(g_snapshot.fetch_add(1)));
  const std::string tmp_path = path + suffix;

  FILE* out = fopen(tmp_path.c_str(), "wb");
  if (!out)
    return false;

  // Every worker keeps appending to its own buffer while this runs, so the
  // snapshot is a consistent prefix-by-worker view rather than a single instant.
  // That is fine: each buffer is append-only, so a concurrent read sees a
  // truncated but valid suffix, and the file is replaced only once complete.
  fprintf(out, "t_ms,kind,worker,edge_key,edge\n");

  // Merge every worker's events into one time-ordered stream.  kMaxEvents is
  // small enough that a sort is cheaper than anything cleverer.
  std::vector<std::pair<int64_t, const Timeline::Event*>> all;
  size_t total = 0;
  for (Buffer& b : g_buffers)
    total += b.events.size();
  all.reserve(total);

  Registry* reg = g_registry;
  auto drain = [&](Buffer& b) {
    for (const Timeline::Event& e : b.events)
      all.emplace_back(e.ticks, &e);
  };
  for (Buffer& b : g_buffers)
    drain(b);
  {
    std::lock_guard<std::mutex> lock(g_fallback_mutex);
    drain(g_fallback);
  }

  std::sort(all.begin(), all.end(),
            [](const std::pair<int64_t, const Timeline::Event*>& a,
               const std::pair<int64_t, const Timeline::Event*>& b) {
              return a.first < b.first;
            });

  // Labels are copied out once, under the registry lock, rather than locking per
  // event: a flush is not on the hot path but it should not serialise the
  // workers either.
  std::unordered_map<int64_t, std::string> labels;
  if (reg) {
    std::lock_guard<std::mutex> lock(reg->mutex);
    labels.reserve(reg->labels.size());
    for (const auto& entry : reg->labels)
      labels[reinterpret_cast<int64_t>(entry.first)] = entry.second;
  }

  for (const auto& item : all) {
    const Timeline::Event& e = *item.second;
    auto it = labels.find(e.edge_key);
    const std::string label =
        (e.edge_key != 0 && it != labels.end()) ? it->second : "<none>";
    fprintf(out, "%.3f,%d,%d,%lld,%s\n", Timeline::TicksToMs(e.ticks),
            static_cast<int>(e.kind), e.worker,
            static_cast<long long>(e.edge_key), label.c_str());
  }

  const bool truncated =
      std::any_of(std::begin(g_buffers), std::end(g_buffers),
                  [](const Buffer& b) { return b.truncated; }) ||
      g_fallback.truncated;

  // fflush plus the atomic rename below is what makes the published file either
  // the previous snapshot or this one, never a partial write.  Without the flush,
  // the rename could publish a file whose bytes are still in the stdio buffer --
  // which is exactly the failure the kill test below exists to catch.
  if (fflush(out) != 0 || ferror(out)) {
    fclose(out);
    DeleteFileA(tmp_path.c_str());
    return false;
  }
  fclose(out);

  if (!MoveFileExA(tmp_path.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileA(tmp_path.c_str());
    return false;
  }

  if (truncated) {
    fprintf(stderr,
            "[sninja] timeline: event buffer truncated; the snapshot is a "
            "prefix, not the whole build\n");
  }
  return true;
}

void FlushAndClose() {
  if (g_flushed.exchange(true))
    return;
  std::lock_guard<std::mutex> lock(g_flush_mutex);
  WriteSnapshot(g_out_path);
}

}  // namespace

bool Timeline::Enabled() {
  static const bool enabled = getenv("SNINJA_TIMELINE") != nullptr;
  return enabled;
}

int64_t Timeline::Now() {
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return counter.QuadPart;
}

void Timeline::Record(Kind kind, int worker, const Edge* edge) {
  if (!Enabled())
    return;

  static const bool initialised = [] {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_freq = freq.QuadPart;
    if (g_freq <= 0)
      g_freq = 1;
    g_registry = new Registry();

    const char* path = getenv("SNINJA_TIMELINE");
    // An unusable path is not fatal: losing the timeline must never break the
    // build it is measuring.  It is reported once, rather than on every flush.
    if (path && *path) {
      g_out_path = path;
      // Confirm the target directory exists and is writable now, while there is
      // still a build to notice the complaint, rather than discovering it at exit.
      const std::string probe = g_out_path + ".probe";
      FILE* test = fopen(probe.c_str(), "wb");
      if (test) {
        fclose(test);
        DeleteFileA(probe.c_str());
      } else {
        fprintf(stderr,
                "[sninja] timeline: cannot write %s; the timeline will be "
                "discarded\n",
                g_out_path.c_str());
        g_out_path.clear();
      }
    }

    const char* interval = getenv("SNINJA_TIMELINE_FLUSH_MS");
    if (interval && *interval) {
      g_flush_interval_ms = strtoll(interval, nullptr, 10);
      if (g_flush_interval_ms <= 0)
        g_flush_interval_ms = 0;  // nonsense value: treat as "not requested"
    }
    g_first = 0;
    return true;
  }();
  (void)initialised;

  const int64_t now = Now();
  if (g_first == 0) {
    // The first event defines t=0.  Written without synchronisation: a race here
    // can only shift t0 by the width of one record, and every other timestamp is
    // absolute anyway.
    g_first = now;
  }

  Event event;
  event.ticks = now;
  event.edge_key = reinterpret_cast<int64_t>(edge);
  event.worker = worker;
  event.kind = static_cast<uint8_t>(kind);

  if (edge && g_registry) {
    std::lock_guard<std::mutex> lock(g_registry->mutex);
    if (g_registry->labels.find(edge) == g_registry->labels.end())
      g_registry->labels[edge] = EdgeLabel(edge);
  }

  Buffer* buffer;
  if (worker >= 0 && worker < kInitialWorkers) {
    buffer = &g_buffers[worker];
  } else {
    std::lock_guard<std::mutex> lock(g_fallback_mutex);
    buffer = &g_fallback;
  }
  if (buffer->events.size() < kMaxEvents)
    buffer->events.push_back(event);
  else
    buffer->truncated = true;

  // Periodic snapshot.  The interval is checked here rather than on a timer
  // thread: a flush costs a few hundred microseconds, so checking once per event
  // is far cheaper than waking a thread to do it, and recording is already
  // happening on every worker so the "last event" time tracks the build closely
  // enough.
  if (g_flush_interval_ms > 0) {
    const int64_t elapsed_ms = Timeline::TicksToMs(now);
    // Only one worker should win the flush, and the lock serialises the write
    // itself.  The comparison is deliberately not atomic: two workers racing here
    // only costs one redundant snapshot attempt, which the lock then serialises.
    if (elapsed_ms - g_last_flush_ms >= g_flush_interval_ms) {
      std::lock_guard<std::mutex> flush_lock(g_flush_mutex);
      if (elapsed_ms - g_last_flush_ms >= g_flush_interval_ms) {
        g_last_flush_ms = elapsed_ms;
        if (!g_out_path.empty())
          WriteSnapshot(g_out_path);
      }
    }
  }
}

void Timeline::Flush() {
  if (!Enabled())
    return;
  static const bool registered = [] {
    atexit(&FlushAndClose);
    return true;
  }();
  (void)registered;
  FlushAndClose();
}

double Timeline::TicksToMs(int64_t ticks) {
  const int64_t delta = ticks - g_first;
  return static_cast<double>(delta) * 1000.0 /
         static_cast<double>(g_freq > 0 ? g_freq : 1);
}
