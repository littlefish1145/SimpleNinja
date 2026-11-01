#include "hash.h"

namespace sninja {

// MurmurHash3-style finalizer based bulk hash. Deterministic within a
// process. Replaced by a SIMD implementation behind runtime dispatch in M5.
size_t HashBytes(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h = 0x9e3779b97f4a7c15ull ^ (static_cast<uint64_t>(len) *
                                        0xff51afd7ed558ccdull);

  auto mix = [](uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return k;
  };

  size_t i = 0;
  for (; i + 8 <= len; i += 8) {
    uint64_t k;
    memcpy(&k, p + i, 8);
    h ^= mix(k);
    h = h * 5 + 0x52dce729ull;
  }
  if (i < len) {
    uint64_t k = 0;
    memcpy(&k, p + i, len - i);
    h ^= mix(k);
    h = h * 5 + 0x38495ab5ull;
  }
  return static_cast<size_t>(mix(h));
}

}  // namespace sninja
