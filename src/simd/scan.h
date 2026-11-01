#ifndef SNINJA_SCAN_H_
#define SNINJA_SCAN_H_

#include <cstddef>

namespace sninja {
namespace scan {

/// Bulk byte scanners used by the lexer. Each function returns a pointer
/// to the first matching byte at or after |p|, or |end| if none found.
/// Implementations are selected at startup: AVX2 > SSE2 > scalar.
using ScanFn = const char* (*)(const char* p, const char* end);

/// First byte of {NUL, '\n', '\r', ' ', '$', ':', '|'} — the read-eval stop set.
extern ScanFn find_eval_stop;
/// First byte that is NOT an identifier char [0-9A-Za-z_.-].
extern ScanFn find_ident_end;
/// First byte that is not ' '.
extern ScanFn find_space_end;
/// First byte of {NUL, '\n'} — comment terminator.
extern ScanFn find_line_end;

/// Force dispatch initialization (idempotent; also runs before main).
void Init();

/// Straightforward scalar implementations, used as the initial values of the
/// pointers above and as the test oracle for the vectorized versions.
namespace reference {
const char* find_eval_stop(const char* p, const char* end);
const char* find_ident_end(const char* p, const char* end);
const char* find_space_end(const char* p, const char* end);
const char* find_line_end(const char* p, const char* end);
}  // namespace reference

}  // namespace scan
}  // namespace sninja

#endif  // SNINJA_SCAN_H_
