#include <gtest/gtest.h>
#include "object_arena.h"
#include "state.h"

namespace {
struct alignas(64) Tracked {
  static inline int alive = 0;
  explicit Tracked(int value) : value(value) { ++alive; }
  ~Tracked() { --alive; }
  int value;
};
}

TEST(ObjectArena, StableAlignedObjectsAndExactDestruction) {
  EXPECT_EQ(0, Tracked::alive);
  {
    sninja::ObjectArena<Tracked, 3> arena;
    std::vector<Tracked*> pointers;
    for (int i = 0; i < 1000; ++i) {
      pointers.push_back(arena.Create(i));
      EXPECT_EQ(0u, reinterpret_cast<uintptr_t>(pointers.back()) % alignof(Tracked));
    }
    for (int i = 0; i < 1000; ++i)
      EXPECT_EQ(i, pointers[i]->value);
    EXPECT_EQ(1000, Tracked::alive);
    arena.Pop(pointers.back());
    EXPECT_EQ(999, Tracked::alive);
    EXPECT_EQ(pointers.back(), arena.Create(123));
    EXPECT_EQ(1000, Tracked::alive);
  }
  EXPECT_EQ(0, Tracked::alive);
}

TEST(NodeStorage, TransientPathKeysSurviveRehashAndCallerMutation) {
  State state;
  std::vector<Node*> pointers;
  for (int i = 0; i < 20000; ++i) {
    std::string path = "long/path/that/is/not/inlined/" + std::to_string(i);
    pointers.push_back(state.GetNode(path, i));
    std::fill(path.begin(), path.end(), 'x');
  }
  for (int i = 0; i < 20000; ++i) {
    std::string path = "long/path/that/is/not/inlined/" + std::to_string(i);
    EXPECT_EQ(pointers[i], state.LookupNode(path));
    EXPECT_EQ(pointers[i], state.GetNode(path, 0));
    EXPECT_EQ(static_cast<uint64_t>(i), pointers[i]->slash_bits());
  }
}

TEST(BindingStorage, ReplacementSortedInsertionAndParentFallback) {
  BindingEnv parent, child(&parent);
  parent.AddBinding("fallback", "parent");
  for (int i = 999; i >= 0; --i)
    child.AddBinding("key" + std::to_string(i), std::to_string(i));
  for (int i = 0; i < 1000; ++i)
    EXPECT_EQ(std::to_string(i), child.LookupVariable("key" + std::to_string(i)));
  child.AddBinding("key30", "changed");
  EXPECT_EQ("changed", child.LookupVariable("key30"));
  EXPECT_EQ("parent", child.LookupVariable("fallback"));
  child.AddBinding("fallback", "");
  EXPECT_EQ("", child.LookupVariable("fallback"));
}
