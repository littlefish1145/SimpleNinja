#ifndef SNINJA_TIMELINE_H_
#define SNINJA_TIMELINE_H_

#include <stdint.h>

// A per-edge execution timeline, for answering questions that aggregate
// counters cannot: what does the shape of a build look like at low -j, how much
// of the wall clock is ramp-up and tail, and which worker is stuck where when a
// build hangs.
//
// Off unless SNINJA_TIMELINE names an output file.  When it is set, every record
// is appended to a per-worker buffer, and the buffer is published as a complete
// CSV snapshot: once at the end of the build, and periodically during it when
// SNINJA_TIMELINE_FLUSH_MS asks for it.
//
// Snapshots are published by writing a temporary file and renaming it over the
// target, so the target is always a whole, readable snapshot -- never a partial
// write.  That matters because the case periodic flushing exists for is a build
// that hangs or gets killed: the file on disk then has to be usable without the
// process ever getting to exit cleanly.
//
// Timestamps are QueryPerformanceCounter ticks.  They are only comparable
// within one process, which is all this is for; TicksToMs converts them using the
// frequency captured at startup.
//
// Note that a snapshot is taken from append-only buffers while workers keep
// writing, so it is a per-worker prefix rather than a single instant: an edge
// whose start is in one snapshot may have its finish in the next.  Events within
// any one snapshot are correctly ordered by time.

struct Edge;

class Timeline {
 public:
  // The kinds of event recorded.  Kept small and explicit so the CSV columns
  // stay interpretable.
  enum class Kind : uint8_t {
    kEdgeStart,   // edge taken by a worker, just before it is launched
    kLaunched,    // subprocess created (after CreateProcess returned)
    kExited,      // child observed to have exited
    kEdgeFinish,  // edge reaped and its bookkeeping done
    kWaitEnter,   // worker parked: nothing to start and nothing to reap
    kWaitExit,    // worker woken
    kFindEmpty,   // a work-stealing scan found no work
    kStuck,       // a worker made no progress for a long time
  };

  // True when SNINJA_TIMELINE is set.  Checked once; the value never changes
  // during a process.
  static bool Enabled();

  /// Monotonic timestamp, in Timeline ticks.
  static int64_t Now();

  /// Append one event.  |edge| may be null for events that are not about a
  /// particular edge (waiting, scanning).
  static void Record(Kind kind, int worker, const Edge* edge);

  /// Write the CSV and release the buffers.  Called automatically at exit, and
  /// safe to call earlier.
  static void Flush();

  /// Convert absolute ticks to milliseconds since the first event recorded.
  static double TicksToMs(int64_t ticks);

  /// One recorded event.  Public only so the recording buffer can hold them; not
  /// part of the interface callers should use.
  struct Event {
    int64_t ticks;
    int64_t edge_key;  // Edge* as an opaque key, or 0 for non-edge events
    int32_t worker;
    uint8_t kind;
  };
};

#endif  // SNINJA_TIMELINE_H_
