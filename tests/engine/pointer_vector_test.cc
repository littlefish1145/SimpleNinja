#include <gtest/gtest.h>
#include <random>
#include "pointer_vector.h"

TEST(PointerVector, RandomMutationMatchesStandardVector) {
  static_assert(sizeof(sninja::PointerVector<int*>) == 3 * sizeof(void*));
  int values[100];
  for (int i = 0; i < 100; ++i) values[i] = i;
  sninja::PointerVector<int*> actual;
  std::vector<int*> expected;
  std::mt19937 random(1107);
  for (int trial = 0; trial < 20000; ++trial) {
    const size_t pos = random() % (expected.size() + 1);
    int* value = &values[random() % 100];
    switch (random() % 8) {
      case 0: actual.push_back(value); expected.push_back(value); break;
      case 1: {
        size_t count = random() % 10;
        actual.insert(actual.begin() + pos, count, value);
        expected.insert(expected.begin() + pos, count, value);
        break;
      }
      case 2: {
        size_t count = expected.size() - pos;
        if (count) count = random() % count;
        actual.erase(actual.begin() + pos, actual.begin() + pos + count);
        expected.erase(expected.begin() + pos, expected.begin() + pos + count);
        break;
      }
      case 3: {
        size_t count = random() % 30;
        actual.resize(count, value); expected.resize(count, value); break;
      }
      case 4: {
        sninja::PointerVector<int*> copied(actual);
        actual = std::move(copied);
        EXPECT_TRUE(copied.empty());
        break;
      }
      case 5: {
        sninja::PointerVector<int*> copied;
        copied = actual;
        sninja::PointerVector<int*> moved(std::move(copied));
        actual = moved;
        break;
      }
      case 6: {
        int* range[] = {value, nullptr, &values[0]};
        actual.insert(actual.begin() + pos, std::begin(range), std::end(range));
        expected.insert(expected.begin() + pos, std::begin(range), std::end(range));
        break;
      }
      case 7: actual.clear(); expected.clear(); break;
    }
    ASSERT_EQ(expected.size(), actual.size());
    ASSERT_TRUE(std::equal(expected.begin(), expected.end(), actual.begin()));
  }
}

TEST(PointerVector, InlineTransitionAndSelfInsertion) {
  int a = 1, b = 2;
  sninja::PointerVector<int*> values;
  const auto* inline_data = values.data();
  values.push_back(&a);
  EXPECT_EQ(inline_data, values.data());
  values.push_back(&b);
  EXPECT_NE(inline_data, values.data());
  values.insert(values.begin() + 1, values.begin(), values.end());
  const int* expected[] = {&a, &a, &b, &b};
  EXPECT_TRUE(std::equal(values.begin(), values.end(), std::begin(expected)));
}
