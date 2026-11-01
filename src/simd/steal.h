// Work-stealing deques used by the parallel build scheduler.
//
// Each worker owns one deque and pushes/pops at the bottom; idle workers steal
// from the top of other workers' deques.  That keeps the common push/pop path
// completely contention-free, while a worker that runs out of work can still
// help itself to another worker's backlog.
//
// Two properties are load-bearing, and both exist because a naive fixed-size
// ring loses work silently under contention:
//
//  1. Indices are never reused.  A thief wins a slot by advancing the shared top
//     index and only then reads that slot.  In a wrapping ring the owner's next
//     push can land on exactly that slot (b - t == capacity), and the thief
//     would read the new item instead of the old one.  So the bottom index only
//     ever grows within a build.
//
//  2. Storage is chunked, so growth never moves memory a thief is reading.  The
//     chunk directory is a fixed-size array allocated up front; only the chunks
//     themselves are allocated on demand, by the owner.
//
// A Plan is created per build, so the storage only has to cover that build's
// pushes.  A worker that reaches the ceiling spills to a shared queue instead.

#ifndef SNINJA_STEAL_H_
#define SNINJA_STEAL_H_

#include <stdint.h>

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace sninja {

/// Try to claim slot |expected| of the shared top index, advancing it by one.
/// Returns true if this caller won the claim.  Implemented in steal.asm.
///
///   bool sninja_claim_top(int64_t* top, int64_t expected)
bool ClaimTopSlot(int64_t* top, int64_t expected);

/// Slots per chunk.  A power of two.
constexpr int64_t kStealChunkShift = 10;
constexpr int64_t kStealChunkSlots = 1 << kStealChunkShift;

/// Ceiling on how many items one worker may hold.  Beyond this, edges spill to
/// the shared queue.  Must be kStealChunkSlots * kStealChunkDirectory.
constexpr int64_t kStealDequeMaxSlots = 1 << 18;
constexpr int64_t kStealChunkDirectory = kStealDequeMaxSlots / kStealChunkSlots;

/// A single-producer / multi-consumer deque of edge pointers.
class StealDeque {
 public:
  StealDeque() = default;

  /// Owner only.  Returns false if the deque is at its ceiling, in which case
  /// the caller should push to the spill queue instead.
  bool Push(void* value);

  /// Owner only.  Pop from the bottom; nullptr if empty.
  void* Pop();

  /// Any thread.  Steal from the top; nullptr if empty.
  void* Steal();

  /// Number of items currently held.
  size_t Size() const {
    const int64_t b = BottomRef().load(std::memory_order_acquire);
    const int64_t t = TopRef().load(std::memory_order_acquire);
    return b > t ? static_cast<size_t>(b - t) : 0;
  }

  bool empty() const { return Size() == 0; }

 private:
  // Laid out as plain int64_t so the assembly can address |top_| with a single
  // LOCK CMPXCHG; every access goes through atomic_ref, so the accesses are
  // well-defined and atomic.  alignas keeps thieves off the owner's line.
  alignas(64) mutable int64_t top_ = 0;
  alignas(64) mutable int64_t bottom_ = 0;

  // The chunk directory is fixed size and never reallocated, so a thief can
  // always index it; the chunks it points at are allocated by the owner and are
  // never moved or freed while the deque is alive.
  std::array<void**, kStealChunkDirectory> chunks_{};

  std::atomic_ref<int64_t> TopRef() const {
    return std::atomic_ref<int64_t>(top_);
  }
  std::atomic_ref<int64_t> BottomRef() const {
    return std::atomic_ref<int64_t>(bottom_);
  }

  static void** SlotAddress(void** chunk, int64_t index) {
    return chunk + (index & (kStealChunkSlots - 1));
  }
};

/// A mutex-protected FIFO used when a worker's deque is full.
class SpillQueue {
 public:
  void Push(void* value);
  /// Take up to |max| items, appending them to |out|.  Returns the count.
  size_t Drain(std::vector<void*>* out, size_t max);
  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

 private:
  mutable std::mutex mutex_;
  std::deque<void*> queue_;
};

}  // namespace sninja

#endif  // SNINJA_STEAL_H_
