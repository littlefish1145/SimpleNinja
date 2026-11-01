// A small, dependency-free reimplementation of the subset of the googletest
// API that the ported ninja test-suites use:
//
//   TEST / TEST_F, EXPECT_{TRUE,FALSE,EQ,NE}, ASSERT_{TRUE,FALSE,EQ,NE},
//   ASSERT_NO_FATAL_FAILURE, testing::Test, --gtest_filter,
//   EXPECT_* << "context".
//
// Values are compared with the same relaxed rules the real gtest uses
// (const char* vs std::string, int vs unsigned, pointers, enums).
#ifndef SNINJA_GTEST_SHIM_H_
#define SNINJA_GTEST_SHIM_H_

#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sninja_testing {

struct Test;

struct TestCase {
  std::string suite;
  std::string name;
  const char* file;
  int line;
  Test* (*create)();
};

/// Failure signalling: a test body returns early on ASSERT_*, and the failure
/// is recorded globally.
bool& CurrentTestFailed();

void ReportFailure(const char* file, int line, const std::string& message);

bool RegisterTest(const char* suite, const char* name, const char* file,
                  int line, Test* (*create)());

std::vector<TestCase>& Registry();

/// Base class for test fixtures and plain tests.
struct Test {
  virtual ~Test() = default;
  virtual void TestBody() = 0;

  /// Runs the body; returns false if it failed.
  bool Run();
};

namespace internal {

template <class T>
inline constexpr bool StringLike =
    std::is_convertible_v<const T&, std::string_view>;

template <class T>
inline constexpr bool IntegralLike =
    std::is_integral_v<std::remove_cv_t<std::remove_reference_t<T>>> ||
    std::is_enum_v<std::remove_cv_t<std::remove_reference_t<T>>>;

template <class T>
inline constexpr bool PointerLike =
    std::is_pointer_v<std::remove_cv_t<std::remove_reference_t<T>>> ||
    std::is_null_pointer_v<std::remove_cv_t<std::remove_reference_t<T>>>;

template <class T>
std::string Print(const T& value) {
  std::ostringstream os;
  using D = std::remove_cv_t<std::remove_reference_t<T>>;
  if constexpr (StringLike<T>) {
    os << '"' << std::string_view(value) << '"';
  } else if constexpr (std::is_same_v<D, bool>) {
    os << (value ? "true" : "false");
  } else if constexpr (std::is_enum_v<D>) {
    os << static_cast<std::underlying_type_t<D>>(value);
  } else if constexpr (PointerLike<T>) {
    os << static_cast<const void*>(value);
  } else if constexpr (requires(std::ostream& o, const D& v) { o << v; }) {
    os << value;
  } else {
    os << "<unprintable value>";
  }
  return os.str();
}

template <class A, class B>
bool Equals(const A& a, const B& b) {
  if constexpr (StringLike<A> && StringLike<B>) {
    return std::string_view(a) == std::string_view(b);
  } else if constexpr (PointerLike<A> && PointerLike<B>) {
    return a == b;
  } else if constexpr (IntegralLike<A> && IntegralLike<B>) {
    using UA = std::remove_cv_t<std::remove_reference_t<A>>;
    using UB = std::remove_cv_t<std::remove_reference_t<B>>;
    if constexpr (std::is_enum_v<UA> || std::is_enum_v<UB>) {
      return a == b;
    } else {
      // std::cmp_equal compares signed/unsigned pairs without the usual
      // sign-conversion surprises (e.g. EXPECT_EQ(1u, some_size)).
      return std::cmp_equal(a, b);
    }
  } else {
    return a == b;
  }
}

template <class A, class B>
std::string CmpFailure(const char* expr_a, const char* expr_b, const char* op,
                       const A& a, const B& b) {
  std::string msg = "Expected: (";
  msg += expr_a;
  msg += ") ";
  msg += op;
  msg += " (";
  msg += expr_b;
  msg += "), actual: ";
  msg += Print(a);
  msg += " vs ";
  msg += Print(b);
  return msg;
}

/// Streaming message builder, so checks can carry extra context:
///   EXPECT_EQ(a, b) << "while parsing " << file;
class Message {
 public:
  template <class T>
  Message& operator<<(const T& value) {
    os_ << value;
    return *this;
  }
  std::string str() const { return os_.str(); }

 private:
  std::ostringstream os_;
};

/// `helper = Message()` reports the failure; assignment is an expression, which
/// is what lets callers append `<< "context"`.
class AssertHelper {
 public:
  AssertHelper(const char* file, int line, std::string message)
      : file_(file), line_(line), message_(std::move(message)) {}

  void operator=(const Message& extra) const {
    std::string text = message_;
    std::string tail = extra.str();
    if (!tail.empty())
      text += "\n  " + tail;
    ReportFailure(file_, line_, text);
  }

 private:
  const char* file_;
  int line_;
  std::string message_;
};

struct AssertionResult {
  bool ok;
  std::string failure_message;
  explicit operator bool() const { return ok; }
};

struct CmpPred {
  template <class A, class B>
  static AssertionResult Eq(const char* ea, const char* eb, const A& a,
                            const B& b) {
    if (Equals(a, b))
      return AssertionResult{true, std::string()};
    return AssertionResult{false, CmpFailure(ea, eb, "==", a, b)};
  }

  template <class A, class B>
  static AssertionResult Ne(const char* ea, const char* eb, const A& a,
                            const B& b) {
    if (!Equals(a, b))
      return AssertionResult{true, std::string()};
    return AssertionResult{false, CmpFailure(ea, eb, "!=", a, b)};
  }
};

}  // namespace internal

}  // namespace sninja_testing

namespace testing {
using Test = sninja_testing::Test;
}  // namespace testing

// The layering below mirrors googletest: every check is an expression ending in
// an `AssertHelper = Message()` statement, which is what makes `<< "context"`
// work and lets fatal checks abort the enclosing function with `return`.
#define SNINJA_AMBIGUOUS_ELSE_BLOCKER_ \
  switch (0)                          \
  case 0:                             \
  default:  // NOLINT

#define SNINJA_NONFATAL_FAILURE_(message)                                 \
  ::sninja_testing::internal::AssertHelper(__FILE__, __LINE__, message) = \
      ::sninja_testing::internal::Message()

#define SNINJA_FATAL_FAILURE_(message) \
  return SNINJA_NONFATAL_FAILURE_(message)

#define SNINJA_PRED_FORMAT2_(pred, v1, v2, on_failure)                  \
  SNINJA_AMBIGUOUS_ELSE_BLOCKER_                                         \
  if (const ::sninja_testing::internal::AssertionResult gtest_ar =      \
          pred(#v1, #v2, v1, v2))                                       \
    ;                                                                   \
  else                                                                  \
    on_failure(gtest_ar.failure_message)

#define SNINJA_PRED_FORMAT1_(value, on_failure)                          \
  SNINJA_AMBIGUOUS_ELSE_BLOCKER_                                         \
  if (value)                                                            \
    ;                                                                   \
  else                                                                  \
    on_failure(std::string("Value of: ") + #value +                      \
               "\n  evaluates to " +                                     \
               ::sninja_testing::internal::Print(static_cast<bool>(value)))

#define EXPECT_EQ(a, b)                                    \
  SNINJA_PRED_FORMAT2_(::sninja_testing::internal::CmpPred::Eq, a, b, \
                       SNINJA_NONFATAL_FAILURE_)
#define EXPECT_NE(a, b)                                    \
  SNINJA_PRED_FORMAT2_(::sninja_testing::internal::CmpPred::Ne, a, b, \
                       SNINJA_NONFATAL_FAILURE_)
#define ASSERT_EQ(a, b)                                    \
  SNINJA_PRED_FORMAT2_(::sninja_testing::internal::CmpPred::Eq, a, b, \
                       SNINJA_FATAL_FAILURE_)
#define ASSERT_NE(a, b)                                    \
  SNINJA_PRED_FORMAT2_(::sninja_testing::internal::CmpPred::Ne, a, b, \
                       SNINJA_FATAL_FAILURE_)

#define EXPECT_TRUE(cond) \
  SNINJA_PRED_FORMAT1_(static_cast<bool>(cond), SNINJA_NONFATAL_FAILURE_)
#define EXPECT_FALSE(cond) \
  SNINJA_PRED_FORMAT1_(!static_cast<bool>(cond), SNINJA_NONFATAL_FAILURE_)
#define ASSERT_TRUE(cond) \
  SNINJA_PRED_FORMAT1_(static_cast<bool>(cond), SNINJA_FATAL_FAILURE_)
#define ASSERT_FALSE(cond) \
  SNINJA_PRED_FORMAT1_(!static_cast<bool>(cond), SNINJA_FATAL_FAILURE_)

/// The ported tests wrap helpers that use ASSERT_* internally; since those
/// helpers abort via `return`, running the statement is equivalent.
#define ASSERT_NO_FATAL_FAILURE(statement) \
  do {                                     \
    statement;                             \
  } while (0)

#define TEST(suite, name)                                             \
  class suite##_##name##_Test : public ::testing::Test {             \
   public:                                                            \
    void TestBody() override;                                         \
    static ::sninja_testing::Test* Create() {                         \
      return new suite##_##name##_Test();                             \
    }                                                                 \
    static const bool g_registered;                                   \
  };                                                                  \
  const bool suite##_##name##_Test::g_registered =                    \
      ::sninja_testing::RegisterTest(#suite, #name, __FILE__,         \
                                     __LINE__,                        \
                                     suite##_##name##_Test::Create);   \
  void suite##_##name##_Test::TestBody()

#define TEST_F(fixture, name)                                         \
  class fixture##_##name##_Test : public fixture {                   \
   public:                                                            \
    void TestBody() override;                                         \
    static ::sninja_testing::Test* Create() {                         \
      return new fixture##_##name##_Test();                           \
    }                                                                 \
    static const bool g_registered;                                   \
  };                                                                  \
  const bool fixture##_##name##_Test::g_registered =                  \
      ::sninja_testing::RegisterTest(#fixture, #name, __FILE__,       \
                                     __LINE__,                        \
                                     fixture##_##name##_Test::Create); \
  void fixture##_##name##_Test::TestBody()

#endif  // SNINJA_GTEST_SHIM_H_
