#include "steal.h"

#include <stdlib.h>

namespace sninja {

extern "C" bool sninja_claim_top(int64_t* top, int64_t expected);

bool ClaimTopSlot(int64_t* top, int64_t expected) {
  return sninja_claim_top(top, expected);
}

bool StealDeque::Push(void* value) {
  const int64_t b = BottomRef().load(std::memory_order_relaxed);
  if (b >= kStealDequeMaxSlots)
    return false;

  // Allocate the chunk if this is the first push into it.  The directory entry
  // is published before the slot, and the bottom (below) is published after
  // both, so a thief that can see the bottom can see the chunk and the slot.
  void**& chunk = chunks_[static_cast<size_t>(b >> kStealChunkShift)];
  if (!chunk)
    chunk = static_cast<void**>(calloc(kStealChunkSlots, sizeof(void*)));

  *SlotAddress(chunk, b) = value;
  // Release: a thief that observes the new bottom must also observe the chunk
  // directory entry and the slot.
  BottomRef().store(b + 1, std::memory_order_release);
  return true;
}

void* StealDeque::Pop() {
  // The owner's region is [top, bottom); a pop takes bottom - 1.
  const int64_t b = BottomRef().load(std::memory_order_relaxed) - 1;

  // Shrink the shared bottom first, so a concurrent thief can see we are taking
  // this index, then order that store before reading the top.
  BottomRef().store(b, std::memory_order_seq_cst);

  const int64_t t = TopRef().load(std::memory_order_relaxed);
  if (t > b) {
    // The region was already empty; put the bottom back.
    BottomRef().store(b + 1, std::memory_order_relaxed);
    return nullptr;
  }

  void** chunk = chunks_[static_cast<size_t>(b >> kStealChunkShift)];
  void** slot = SlotAddress(chunk, b);

  if (t == b) {
    // The region held exactly one item, so a thief may be racing us for it.
    // The compare-exchange decides the owner; top becomes b + 1 either way,
    // which is exactly the new bottom.
    const bool won = ClaimTopSlot(&top_, b);
    BottomRef().store(b + 1, std::memory_order_relaxed);
    if (!won)
      return nullptr;
    void* value = *slot;
    *slot = nullptr;
    return value;
  }

  // More than one item: no thief can have reached index b, so no claim needed.
  void* value = *slot;
  *slot = nullptr;
  // Leave the bottom at b, so the next pop takes b - 1.
  BottomRef().store(b, std::memory_order_relaxed);
  return value;
}

void* StealDeque::Steal() {
  // The order of these operations is load-bearing and must not be rearranged.
  //
  // A thief must observe the top index *before* the bottom index, with a
  // sequentially consistent fence in between.  The owner's pop publishes a
  // shrunken bottom and then fences; putting the thief's read of the top before
  // that same fence places both in one total order.  Without it a thief could
  // read a bottom that is already stale (the owner popped and pushed again),
  // conclude there is something at index t, and claim an index that no longer
  // refers to a live item.
  const int64_t t = TopRef().load(std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  const int64_t b = BottomRef().load(std::memory_order_acquire);
  if (t >= b)
    return nullptr;

  if (!ClaimTopSlot(&top_, t))
    return nullptr;  // somebody else took index t; the caller will retry

  // Claimed, so this index is ours alone: nobody else can claim it, and because
  // indices are never reused, no later push can overwrite the slot either.
  void** chunk = chunks_[static_cast<size_t>(t >> kStealChunkShift)];
  void** slot = SlotAddress(chunk, t);
  void* value = *slot;
  *slot = nullptr;
  return value;
}

void SpillQueue::Push(void* value) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back(value);
}

size_t SpillQueue::Drain(std::vector<void*>* out, size_t max) {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t n = 0;
  while (n < max && !queue_.empty()) {
    out->push_back(queue_.front());
    queue_.pop_front();
    ++n;
  }
  return n;
}

}  // namespace sninja
