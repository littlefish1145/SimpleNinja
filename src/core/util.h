#ifndef SNINJA_UTIL_H_
#define SNINJA_UTIL_H_

#include <stdarg.h>

#include <cstdint>
#include <string>

#if __has_cpp_attribute(noreturn)
#define NORETURN [[noreturn]]
#else
#define NORETURN
#endif

/// Log a fatal message and exit.
NORETURN void Fatal(const char* msg, ...);

/// Log a warning message.
void Warning(const char* msg, ...);
void Warning(const char* msg, va_list ap);

/// Log an error message.
void Error(const char* msg, ...);
void Error(const char* msg, va_list ap);

/// Log an informational message.
void Info(const char* msg, ...);
void Info(const char* msg, va_list ap);

/// Canonicalize a path like "foo/../bar.h" into just "bar.h".
/// |slash_bits| has bits set starting from lowest for a backslash that was
/// normalized to a forward slash. (only used on Windows)
void CanonicalizePath(std::string* path, uint64_t* slash_bits);
void CanonicalizePath(char* path, size_t* len, uint64_t* slash_bits);

/// Appends |input| to |*result|, escaping according to the whims of either
/// Bash, or Win32's CommandLineToArgvW().
void GetShellEscapedString(const std::string& input, std::string* result);
void GetWin32EscapedString(const std::string& input, std::string* result);

/// Read a file to a string.  Returns 0 on success, -errno otherwise.
int ReadFile(const std::string& path, std::string* contents, std::string* err);

/// Mark a file descriptor close-on-exec.  No-op on Windows, where descriptors
/// are not inherited by default.
void SetCloseOnExec(int fd);

/// Move |tmp_dst| over |file_dst|, deleting the previous contents of
/// |file_dst|.  Used to make log rewrites atomic.
bool AtomicReplace(const std::string& file_dst, const std::string& tmp_dst,
                 std::string* err);

/// Truncate a file to the given size.  Returns false on failure.
bool Truncate(const std::string& path, size_t size, std::string* err);

/// Remove |filename|.  Returns 0 on success, -1 on failure (and errno set).
int platformAwareUnlink(const char* filename);

/// The number of hardware threads available to this process.
/// Mark the descriptor behind |fp| non-inheritable, so it is not leaked into
/// child processes (see Subprocess::Start, which passes bInheritHandles).
void MakeNonInheritable(FILE* fp);

/// How many handles this process currently has open.  Used to check the cost of
/// CreateProcess(bInheritHandles = TRUE), which duplicates all of them into the
/// child.
int GetProcessHandleCount();

int GetProcessorCount();

/// The default number of parallel jobs: the number of performance cores on a
/// hybrid CPU, otherwise the number of logical processors.
int GetDefaultParallelism();

#ifdef _WIN32
/// Split the machine's cores into performance cores (|pcores|) and efficiency
/// cores (|ecores|) using the CPU set information.  Returns false when the CPU
/// does not report hybrid topology.
bool GetHybridCoreCounts(int* pcores, int* ecores);
#endif

/// Milliseconds from a monotonic clock; only differences are meaningful.
int64_t NowMillis();

/// Put the MSVC toolset's LIB/INCLUDE/PATH into this process's environment so
/// that child processes (cl.exe, link.exe) can find the compiler and the
/// Windows SDK libraries.  No-op when no Visual Studio installation is found.
void SetupMsvcEnvironment();

/// The current working directory; Fatal()s if it cannot be determined.
std::string GetWorkingDirectory();

#ifdef _WIN32
/// Convert the value returned by GetLastError() into a string.
std::string GetLastErrorString();
#endif

#endif  // SNINJA_UTIL_H_
