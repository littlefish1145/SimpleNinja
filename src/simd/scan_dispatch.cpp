#include "scan.h"

#include <mutex>

#ifdef _WIN32
#include <intrin.h>
#endif

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

namespace sninja {
namespace scan {

namespace {

const char* RefFindEvalStop(const char* p, const char* end) {
  while (p < end) {
    char c = *p;
    // Stop set: NUL, '\n', '\r', ' ', '$', ':', '|'
    if (c == 0 || c == '\n' || c == '\r' || c == ' ' || c == '$' ||
        c == ':' || c == '|')
      return p;
    ++p;
  }
  return p;
}
constexpr bool IsIdentChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') || c == '_' || c == '.' || c == '-';
}

const char* RefFindIdentEnd(const char* p, const char* end) {
  while (p < end && IsIdentChar(*p)) ++p;
  return p;
}

const char* RefFindSpaceEnd(const char* p, const char* end) {
  while (p < end && *p == ' ') ++p;
  return p;
}

const char* RefFindLineEnd(const char* p, const char* end) {
  while (p < end && *p != 0 && *p != '\n') ++p;
  return p;
}

#ifdef _WIN32
bool HasAVX2() {
  int info[4];
  __cpuid(info, 1);
  const bool osxsave = (info[2] & (1 << 27)) != 0;
  const bool avx = (info[2] & (1 << 28)) != 0;
  if (!osxsave || !avx)
    return false;
  if ((_xgetbv(0) & 0x6) != 0x6)
    return false;
  __cpuidex(info, 7, 0);
  return (info[1] & (1 << 5)) != 0;
}
#else
bool HasAVX2() { return false; }
#endif

void InitImpl() {
  find_eval_stop = sn_sse2_find_eval_stop;
  find_ident_end = sn_sse2_find_ident_end;
  find_space_end = sn_sse2_find_space_end;
  find_line_end = sn_sse2_find_line_end;
  if (HasAVX2()) {
    find_eval_stop = sn_avx2_find_eval_stop;
    find_ident_end = sn_avx2_find_ident_end;
    find_space_end = sn_avx2_find_space_end;
    find_line_end = sn_avx2_find_line_end;
  }
}

struct AutoInit {
  AutoInit() { Init(); }
};
const AutoInit g_auto_init;

}  // namespace

const char* reference::find_eval_stop(const char* p, const char* end) {
  return RefFindEvalStop(p, end);
}
const char* reference::find_ident_end(const char* p, const char* end) {
  return RefFindIdentEnd(p, end);
}
const char* reference::find_space_end(const char* p, const char* end) {
  return RefFindSpaceEnd(p, end);
}
const char* reference::find_line_end(const char* p, const char* end) {
  return RefFindLineEnd(p, end);
}

ScanFn find_eval_stop = RefFindEvalStop;
ScanFn find_ident_end = RefFindIdentEnd;
ScanFn find_space_end = RefFindSpaceEnd;
ScanFn find_line_end = RefFindLineEnd;

void Init() {
  static std::once_flag once;
  std::call_once(once, InitImpl);
}

}  // namespace scan
}  // namespace sninja
