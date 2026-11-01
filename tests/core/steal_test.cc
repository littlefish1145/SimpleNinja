#include "steal.h"

#include <stdio.h>

#include <atomic>
#include <set>
#include <thread>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"

using namespace sninja;

namespace {

// Stand-in payloads: the deque only ever moves opaque pointers around.  One
// distinct address per item, so a test can tell items apart and detect both
// duplicates and losses.
constexpr int kPayloadCount = 40000;
char payload_pool[kPayloadCount * sizeof(int)];

void* Payload(int i) { return payload_pool + i * sizeof(int); }

int PayloadIndex(void* p) {
  return static_cast<int>(static_cast<char*>(p) - payload_pool) /
         static_cast<int>(sizeof(int));
}


// The assembly entry point itself: it must advance |top| by exactly one and
// report success to exactly one of two callers racing for the same slot.
TEST(Steal, ClaimTopSlotAdvancesOnce) {
  alignas(64) int64_t top = 0;
  EXPECT_TRUE(ClaimTopSlot(&top, 0));
  EXPECT_EQ(1, top);
  // The expected value is stale now, so the claim must fail.
  EXPECT_FALSE(ClaimTopSlot(&top, 0));
  EXPECT_EQ(1, top);
  EXPECT_TRUE(ClaimTopSlot(&top, 1));
  EXPECT_EQ(2, top);
}

TEST(Steal, ClaimTopSlotAdmitsExactlyOneWinner) {
  constexpr int kRacers = 8;
  constexpr int kRounds = 2000;

  for (int round = 0; round < kRounds; ++round) {
    alignas(64) int64_t top = 0;
    std::atomic<int> winners{0};
    std::vector<std::thread> racers;
    for (int i = 0; i < kRacers; ++i) {
      racers.emplace_back([&] {
        if (ClaimTopSlot(&top, 0))
          winners.fetch_add(1, std::memory_order_relaxed);
      });
    }
    for (std::thread& t : racers)
      t.join();
    // Every round must hand the single slot to exactly one racer, and the index
    // must advance exactly once.
    ASSERT_EQ(1, winners.load()) << "round " << round;
    ASSERT_EQ(1, top) << "round " << round;
  }
}

TEST(Steal, PushThenPopIsLifo) {
  StealDeque deque;
  EXPECT_TRUE(deque.Push(Payload(1)));
  EXPECT_TRUE(deque.Push(Payload(2)));
  EXPECT_TRUE(deque.Push(Payload(3)));
  EXPECT_EQ(3u, deque.Size());

  EXPECT_EQ(Payload(3), deque.Pop());
  EXPECT_EQ(Payload(2), deque.Pop());
  EXPECT_EQ(Payload(1), deque.Pop());
  EXPECT_EQ(nullptr, deque.Pop());
  EXPECT_EQ(0u, deque.Size());
}

TEST(Steal, PushPopStealRepeatedlyNeverLosesAnItem) {
  // Drive the deque through many push/pop/steal cycles on one thread.  If pop's
  // bookkeeping of the bottom index were off by one, an item would either be
  // handed out twice or dropped.
  StealDeque deque;
  constexpr int kCycles = 3000;
  int next_push = 0;
  int popped = 0;
  int stolen = 0;
  for (int cycle = 0; cycle < kCycles; ++cycle) {
    for (int i = 0; i < 3; ++i)
      ASSERT_TRUE(deque.Push(Payload(next_push++)));
    if (void* p = deque.Steal())
      ++stolen;
    if (void* p = deque.Pop())
      ++popped;
  }
  // Drain whatever is left.
  while (void* p = deque.Pop())
    ++popped;
  while (void* p = deque.Steal())
    ++stolen;
  EXPECT_EQ(next_push, popped + stolen)
      << "pushed " << next_push << " but consumed " << popped + stolen;
}


TEST(Steal, StealTakesFromTheOldestEnd) {
  StealDeque deque;
  deque.Push(Payload(1));
  deque.Push(Payload(2));
  deque.Push(Payload(3));

  // Stealing runs FIFO so that the longest-waiting edges go first.
  EXPECT_EQ(Payload(1), deque.Steal());
  EXPECT_EQ(Payload(2), deque.Steal());
  EXPECT_EQ(Payload(3), deque.Steal());
  EXPECT_EQ(nullptr, deque.Steal());
}

TEST(Steal, StealOnEmptyDequeReturnsNull) {
  StealDeque deque;
  EXPECT_EQ(nullptr, deque.Steal());
  deque.Push(Payload(7));
  EXPECT_EQ(Payload(7), deque.Steal());
  EXPECT_EQ(nullptr, deque.Steal());
}

TEST(Steal, PopAndStealSplitTheContents) {
  StealDeque deque;
  for (int i = 0; i < 8; ++i)
    deque.Push(Payload(i));

  // With more than one item the owner wins without contending.
  EXPECT_EQ(Payload(7), deque.Pop());
  EXPECT_EQ(Payload(0), deque.Steal());
  EXPECT_EQ(Payload(1), deque.Steal());
  EXPECT_EQ(Payload(6), deque.Pop());
  EXPECT_EQ(Payload(2), deque.Steal());
}

TEST(Steal, PushReportsFullDeque) {
  StealDeque deque;
  for (int64_t i = 0; i < kStealDequeMaxSlots; ++i)
    ASSERT_TRUE(deque.Push(Payload(static_cast<int>(i)))) << "at " << i;
  // At the ceiling the deque refuses more and the caller spills to the shared
  // queue.
  EXPECT_FALSE(deque.Push(Payload(0)));
}

TEST(Steal, PopAfterManyPushesStillWalksBackwards) {
  // Indices are never reused, so this also checks that the buffer grew and that
  // pop still takes the newest item after growth.
  StealDeque deque;
  const int kItems = 5000;
  for (int i = 0; i < kItems; ++i)
    ASSERT_TRUE(deque.Push(Payload(i)));
  EXPECT_EQ(static_cast<size_t>(kItems), deque.Size());
  for (int i = kItems - 1; i >= 0; --i)
    EXPECT_EQ(Payload(i), deque.Pop()) << "at " << i;
  EXPECT_EQ(nullptr, deque.Pop());
}


// Hammer one deque from several thieves while the owner pushes and pops.  Every
// pushed item must be observed exactly once, by either the owner or a thief.
TEST(Steal, ConcurrentPushPopAndStealLosesNothing) {
  const int kItems = 20000;
  const int kThieves = 3;

  StealDeque deque;
  std::vector<std::atomic<bool>> taken(kItems);
  for (auto& flag : taken)
    flag.store(false);

  std::atomic<int> consumed{0};
  std::atomic<bool> producer_done{false};
  std::atomic<int> duplicates{0};

  std::vector<std::thread> thieves;
  for (int t = 0; t < kThieves; ++t) {
    thieves.emplace_back([&] {
      while (!producer_done.load(std::memory_order_acquire)) {
        void* p = deque.Steal();
        if (!p)
          continue;
        int index = static_cast<int>(PayloadIndex(p));
        bool expected = false;
        if (!taken[index].compare_exchange_strong(expected, true))
          ++duplicates;
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  for (int i = 0; i < kItems; ++i) {
    while (!deque.Push(Payload(i)))
      std::this_thread::yield();
  }
  producer_done.store(true, std::memory_order_release);

  // Drain whatever the thieves left behind.
  while (void* p = deque.Pop()) {
    int index = static_cast<int>(PayloadIndex(p));
    bool expected = false;
    if (!taken[index].compare_exchange_strong(expected, true))
      ++duplicates;
    consumed.fetch_add(1, std::memory_order_relaxed);
  }
  for (std::thread& t : thieves)
    t.join();


  EXPECT_EQ(0, duplicates.load());
  EXPECT_EQ(kItems, consumed.load());
  for (int i = 0; i < kItems; ++i)
    ASSERT_TRUE(taken[i].load()) << "item " << i << " was lost";
}

TEST(SpillQueue, DrainsInOrderAndRespectsTheLimit) {
  SpillQueue queue;
  EXPECT_TRUE(queue.empty());
  for (int i = 0; i < 5; ++i)
    queue.Push(Payload(i));

  std::vector<void*> out;
  EXPECT_EQ(2u, queue.Drain(&out, 2));
  EXPECT_EQ(Payload(0), out[0]);
  EXPECT_EQ(Payload(1), out[1]);
  EXPECT_FALSE(queue.empty());

  EXPECT_EQ(3u, queue.Drain(&out, 10));
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(0u, queue.Drain(&out, 10));
}

}  // namespace
