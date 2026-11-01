#include <gtest/gtest.h>
#include <array>
#include <random>
#include <string>
#include "scan.h"
#include "lexer.h"
#include "manifest_path.h"
#include "manifest_parser.h"
#include "state.h"

template <bool Path>
void CheckScanner() {
  std::mt19937 random(0x1234);
  std::array<char, 320> bytes;
  for (int trial = 0; trial < 2000; ++trial) {
    for (char& c : bytes)
      c = static_cast<char>(random());
    for (size_t offset = 0; offset < 32; ++offset) {
      size_t length = random() % (bytes.size() - offset);
      const char* begin = bytes.data() + offset;
      const char* expected = begin;
      while (expected != begin + length) {
        char c = *expected;
        if (c == 0 || c == '$' || c == '\r' || c == '\n' ||
            (Path && (c == ' ' || c == ':' || c == '|')))
          break;
        ++expected;
      }
      ASSERT_EQ(expected, sninja::ScanLiteral<Path>(std::span(begin, length)));
    }
  }
}

TEST(BoundedScanner, ArbitraryBytesAndUnalignedRanges) {
  CheckScanner<true>();
  CheckScanner<false>();
  // Heap-sized buffers end at exactly the supplied range (ASan checks tails).
  for (size_t length = 1; length < 160; ++length) {
    auto bytes = std::make_unique<char[]>(length);
    std::fill_n(bytes.get(), length, 'x');
    EXPECT_EQ(bytes.get() + length,
              sninja::ScanLiteral<true>(std::span(bytes.get(), length)));
    for (int value = 0; value < 256; ++value) {
      bytes[length - 1] = static_cast<char>(value);
      const bool stop = value == 0 || value == '$' || value == '\r' ||
          value == '\n' || value == ' ' || value == ':' || value == '|';
      EXPECT_EQ(bytes.get() + length - stop,
                sninja::ScanLiteral<true>(std::span(bytes.get(), length)));
    }
  }
}

TEST(ManifestPath, RetainsDeferredEdgeBindings) {
  State state;
  ManifestParser parser(&state, nullptr);
  std::string error;
  ASSERT_TRUE(parser.ParseTest(
      "rule cc\n  command = echo $in $out\n"
      "base = outer\n"
      "build literal/very_long_output_name $base/out: cc literal/very_long_input_name $base/in\n"
      "  base = inner\n", &error)) << error;
  ASSERT_EQ(1u, state.edges_.size());
  const Edge* edge = state.edges_[0];
  EXPECT_EQ("inner/out", edge->outputs_[1]->path());
  EXPECT_EQ("inner/in", edge->inputs_[1]->path());
  EXPECT_EQ("literal/very_long_output_name", edge->outputs_[0]->path());
}

TEST(ManifestPath, BorrowedCanonicalPathsMatchFullNormalizer) {
  std::mt19937 random(739);
  const std::string alphabet = "abcd./\\ :_-";
  BindingEnv env;
  for (int trial = 0; trial < 50000; ++trial) {
    std::string input;
    size_t length = 1 + random() % 140;
    for (size_t i = 0; i < length; ++i)
      input += alphabet[random() % alphabet.size()];
    ManifestPath path;
    path.value = std::string_view(input);
    std::string expected = input, scratch;
    uint64_t expected_slashes, actual_slashes;
    CanonicalizePath(&expected, &expected_slashes);
    auto actual = path.EvaluateCanonical(&env, &scratch, &actual_slashes);
    ASSERT_EQ(expected, actual.AsString()) << input;
    ASSERT_EQ(expected_slashes, actual_slashes) << input;
  }
}

TEST(ManifestPath, MatchesOriginalLexerAtEveryBoundary) {
  const std::string cases[] = {
      "", "plain", "plain\n", "plain\r\n", "plain\r", "with$ space\n",
      "prefix$variable\n", "prefix${variable}/suffix\n", "a$:b\n",
      "a$\n  continued\n", "a$\r\n  continued\n", "a$^b\n", "a\tb : next\n",
      std::string("abc\0def\n", 8), "a|other\n", "a:other\n", "a$$b\n",
      "x $\n  y\n", "x\r\ny\n", "x\ry\n"};
  BindingEnv env;
  env.AddBinding("variable", "replaced");
  for (const auto& input : cases) {
    Lexer original(input.c_str()), optimized(input.c_str());
    EvalString eval;
    ManifestPath path;
    std::string first_error, second_error;
    bool a = original.ReadPath(&eval, &first_error);
    bool b = optimized.ReadPath(&path, &second_error);
    EXPECT_EQ(a, b) << input;
    EXPECT_EQ(first_error, second_error) << input;
    if (a) {
      EXPECT_EQ(eval.Evaluate(&env), path.Evaluate(&env)) << input;
      EXPECT_EQ(original.ReadToken(), optimized.ReadToken()) << input;
    }
  }
}
