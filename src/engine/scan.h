// sninja: bounded SIMD scanning. No read may cross the provided input range.
#ifndef SNINJA_ENGINE_SCAN_H_
#define SNINJA_ENGINE_SCAN_H_
#include <bit>
#include <span>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace sninja {
template <bool Path>
inline const char* ScanLiteral(std::span<const char> input) {
  const char* p = input.data();
  const char* end = p + input.size();
#if defined(__SSE2__) || defined(_M_X64)
  while (end - p >= 16) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
    __m128i special = _mm_or_si128(_mm_cmpeq_epi8(v, _mm_setzero_si128()),
                                  _mm_cmpeq_epi8(v, _mm_set1_epi8('$')));
    special = _mm_or_si128(special, _mm_cmpeq_epi8(v, _mm_set1_epi8('\n')));
    special = _mm_or_si128(special, _mm_cmpeq_epi8(v, _mm_set1_epi8('\r')));
    if constexpr (Path) {
      special = _mm_or_si128(special, _mm_cmpeq_epi8(v, _mm_set1_epi8(' ')));
      special = _mm_or_si128(special, _mm_cmpeq_epi8(v, _mm_set1_epi8(':')));
      special = _mm_or_si128(special, _mm_cmpeq_epi8(v, _mm_set1_epi8('|')));
    }
    unsigned mask = static_cast<unsigned>(_mm_movemask_epi8(special));
    if (mask)
      return p + std::countr_zero(mask);
    p += 16;
  }
#endif
  while (p != end) {
    char c = *p;
    if (c == 0 || c == '$' || c == '\n' || c == '\r')
      break;
    if constexpr (Path)
      if (c == ' ' || c == ':' || c == '|')
        break;
    ++p;
  }
  return p;
}
}  // namespace sninja
#endif
