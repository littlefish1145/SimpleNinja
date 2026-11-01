// Differential test for the vectorized lexer scanners.
//
// Every vectorized entry point must agree with the scalar reference, in
// particular for stop characters that land exactly on a vector-block boundary
// and for start offsets that shift the alignment (this is where a missing
// scalar-tail case hides).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "scan.h"

extern "C" {
const char* sn_sse2_find_eval_stop(const char* p, const char* end);
const char* sn_sse2_find_ident_end(const char* p, const char* end);
const char* sn_sse2_find_space_end(const char* p, const char* end);
const char* sn_sse2_find_line_end(const char* p, const char* end);
const char* sn_avx2_find_eval_stop(const char* p, const char* end);
const char* sn_avx2_find_ident_end(const char* p, const char* end);
const char* sn_avx2_find_space_end(const char* p, const char* end);
const char* sn_avx2_find_line_end(const char* p, const char* end);
}

namespace {

using sninja::scan::ScanFn;
namespace ref = sninja::scan::reference;

struct Variant {
  const char* name;
  ScanFn eval_stop;
  ScanFn ident_end;
  ScanFn space_end;
  ScanFn line_end;
};

std::vector<Variant> Variants() {
  sninja::scan::Init();
  return {
      {"sse2", sn_sse2_find_eval_stop, sn_sse2_find_ident_end,
       sn_sse2_find_space_end, sn_sse2_find_line_end},
      {"avx2", sn_avx2_find_eval_stop, sn_avx2_find_ident_end,
       sn_avx2_find_space_end, sn_avx2_find_line_end},
      {"dispatched", sninja::scan::find_eval_stop, sninja::scan::find_ident_end,
       sninja::scan::find_space_end, sninja::scan::find_line_end},
  };
}

/// Scan |data| over the whole buffer, for a handful of start offsets so that
/// the vector blocks are exercised at several alignments.
void CheckVariant(const Variant& v, const std::string& data) {
  for (size_t start = 0; start < 4 && start <= data.size(); ++start) {
    const char* p = data.data() + start;
    const char* end = data.data() + data.size();
    EXPECT_EQ(ref::find_eval_stop(p, end), v.eval_stop(p, end))
        << v.name << " eval_stop start=" << start;
    EXPECT_EQ(ref::find_ident_end(p, end), v.ident_end(p, end))
        << v.name << " ident_end start=" << start;
    EXPECT_EQ(ref::find_space_end(p, end), v.space_end(p, end))
        << v.name << " space_end start=" << start;
    EXPECT_EQ(ref::find_line_end(p, end), v.line_end(p, end))
        << v.name << " line_end start=" << start;
  }
}

/// Exhaustive: every (start, end) sub-range.  Only for small buffers.
void CheckVariantExhaustive(const Variant& v, const std::string& data) {
  for (size_t start = 0; start <= data.size(); ++start) {
    for (size_t len = 0; len <= data.size() - start; ++len) {
      const char* p = data.data() + start;
      const char* end = p + len;
      EXPECT_EQ(ref::find_eval_stop(p, end), v.eval_stop(p, end))
          << v.name << " eval_stop start=" << start << " len=" << len;
      EXPECT_EQ(ref::find_ident_end(p, end), v.ident_end(p, end))
          << v.name << " ident_end start=" << start << " len=" << len;
      EXPECT_EQ(ref::find_space_end(p, end), v.space_end(p, end))
          << v.name << " space_end start=" << start << " len=" << len;
      EXPECT_EQ(ref::find_line_end(p, end), v.line_end(p, end))
          << v.name << " line_end start=" << start << " len=" << len;
    }
  }
}

}  // namespace

TEST(Scan, MatchesScalarReferenceOnIdentLikeData) {
  const std::string data =
      "rule cc\n"
      "  command = clang -c $in -o $out\n"
      "build out/foo.o: cc src/foo.cc\n";
  for (const Variant& v : Variants())
    CheckVariant(v, data);
}

TEST(Scan, MatchesScalarReferenceAcrossVectorBoundaries) {
  // A stop character at every offset around the 16/32-byte block boundaries.
  for (size_t offset = 0; offset < 80; ++offset) {
    for (char stop : {'$', ' ', '\n', '\r', ':', '|'}) {
      std::string data(offset, 'a');
      data.push_back(stop);
      data.append(48, 'b');
      for (const Variant& v : Variants())
        CheckVariant(v, data);
    }
  }
}

TEST(Scan, MatchesScalarReferenceOnAllByteValues) {
  for (size_t offset = 0; offset < 4; ++offset) {
    std::string data(offset, 'x');
    for (int b = 1; b < 256; ++b)
      data.push_back(static_cast<char>(b));
    data.append(64, 'y');
    for (const Variant& v : Variants())
      CheckVariant(v, data);
  }
}

TEST(Scan, MatchesScalarReferenceWithEmbeddedNul) {
  // The scanners are bounded by |end| only, so an embedded NUL is an ordinary
  // byte that happens to be a member of every stop set.
  const std::string data("abc\0def\0ghi", 11);
  for (const Variant& v : Variants())
    CheckVariantExhaustive(v, data);
}

TEST(Scan, MatchesScalarReferenceOnShortRanges) {
  for (const std::string& data : {std::string(""), std::string("a"),
                                  std::string("ab"), std::string("abc")}) {
    for (const Variant& v : Variants())
      CheckVariantExhaustive(v, data);
  }
}

TEST(Scan, MatchesScalarReferenceOnTrickyCharacters) {
  const std::string data = "abc.DEF-123_ x/y::z\t$${foo.bar}-$ \t\r\n|";
  for (const Variant& v : Variants())
    CheckVariant(v, data);
}
