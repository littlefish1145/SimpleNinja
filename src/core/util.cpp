#include "util.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <assert.h>
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <utility>
#include <vector>

using namespace std;


#include <algorithm>

#if __has_cpp_attribute(fallthrough)
#define NINJA_FALLTHROUGH [[fallthrough]]
#else
#define NINJA_FALLTHROUGH
#endif

void Fatal(const char* msg, ...) {
  va_list ap;
  fprintf(stderr, "ninja: fatal: ");
  va_start(ap, msg);
  vfprintf(stderr, msg, ap);
  va_end(ap);
  fprintf(stderr, "\n");
#ifdef _WIN32
  // On Windows, some tools may inject extra threads.
  // exit() may block on locks held by those threads, so forcibly exit.
  fflush(stderr);
  fflush(stdout);
  ExitProcess(1);
#else
  exit(1);
#endif
}

void Warning(const char* msg, va_list ap) {
  fprintf(stderr, "ninja: warning: ");
  vfprintf(stderr, msg, ap);
  fprintf(stderr, "\n");
}

void Warning(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  Warning(msg, ap);
  va_end(ap);
}

void Error(const char* msg, va_list ap) {
  fprintf(stderr, "ninja: error: ");
  vfprintf(stderr, msg, ap);
  fprintf(stderr, "\n");
}

void Error(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  Error(msg, ap);
  va_end(ap);
}

void Info(const char* msg, va_list ap) {
  fprintf(stdout, "ninja: ");
  vfprintf(stdout, msg, ap);
  fprintf(stdout, "\n");
}

void Info(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  Info(msg, ap);
  va_end(ap);
}

void CanonicalizePath(std::string* path, uint64_t* slash_bits) {
  size_t len = path->size();
  char* str = 0;
  if (len > 0)
    str = &(*path)[0];
  CanonicalizePath(str, &len, slash_bits);
  path->resize(len);
}

static bool IsPathSeparator(char c) {
#ifdef _WIN32
  return c == '/' || c == '\\';
#else
  return c == '/';
#endif
}

void CanonicalizePath(char* path, size_t* len, uint64_t* slash_bits) {
  // WARNING: this function is performance-critical; please benchmark
  // any changes you make to it.
  if (*len == 0) {
    return;
  }

  char* start = path;
  char* dst = start;
  char* dst_start = dst;
  const char* src = start;
  const char* end = start + *len;
  const char* src_next;

  // For absolute paths, skip the leading directory separator
  // as this one should never be removed from the result.
  if (IsPathSeparator(*src)) {
#ifdef _WIN32
    // Windows network path starts with //
    if (src + 2 <= end && IsPathSeparator(src[1])) {
      src += 2;
      dst += 2;
    } else {
      ++src;
      ++dst;
    }
#else
    ++src;
    ++dst;
#endif
    dst_start = dst;
  } else {
    // For relative paths, skip any leading ../ as these are quite common
    // to reference source files in build plans, and doing this here makes
    // the loop work below faster in general.
    while (src + 3 <= end && src[0] == '.' && src[1] == '.' &&
           IsPathSeparator(src[2])) {
      src += 3;
      dst += 3;
    }
  }

  // Loop over all components of the paths _except_ the last one, in
  // order to simplify the loop's code and make it faster.
  int component_count = 0;
  char* dst0 = dst;
  for (; src < end; src = src_next) {
#ifndef _WIN32
    const char* next_sep =
        static_cast<const char*>(::memchr(src, '/', end - src));
    if (!next_sep) {
      break;
    }
#else
    // Need to check for both '/' and '\\' so do not use memchr().
    // Cannot use strpbrk() because end[0] can be \0 or something else!
    const char* next_sep = src;
    while (next_sep != end && !IsPathSeparator(*next_sep))
      ++next_sep;
    if (next_sep == end) {
      // This is the last component, will be handled out of the loop.
      break;
    }
#endif
    // Position for next loop iteration.
    src_next = next_sep + 1;
    // Length of the component, excluding trailing directory.
    size_t component_len = next_sep - src;

    if (component_len <= 2) {
      if (component_len == 0) {
        continue;  // Ignore empty component, e.g. 'foo//bar' -> 'foo/bar'.
      }
      if (src[0] == '.') {
        if (component_len == 1) {
          continue;  // Ignore '.' component, e.g. './foo' -> 'foo'.
        } else if (src[1] == '.') {
          // Process the '..' component if found. Back up if possible.
          if (component_count > 0) {
            // Move back to start of previous component.
            --component_count;
            while (--dst > dst0 && !IsPathSeparator(dst[-1])) {
              // nothing to do here, decrement happens before condition check.
            }
          } else {
            dst[0] = '.';
            dst[1] = '.';
            dst[2] = src[2];
            dst += 3;
          }
          continue;
        }
      }
    }
    ++component_count;

    // Copy or skip component, including trailing directory separator.
    if (dst != src) {
      ::memmove(dst, src, src_next - src);
    }
    dst += src_next - src;
  }

  // Handling the last component that does not have a trailing separator.
  // The logic here is _slightly_ different since there is no trailing
  // directory separator.
  size_t component_len = end - src;
  do {
    if (component_len == 0)
      break;  // Ignore empty component (e.g. 'foo//' -> 'foo/')
    if (src[0] == '.') {
      if (component_len == 1)
        break;  // Ignore trailing '.' (e.g. 'foo/.' -> 'foo/')
      if (component_len == 2 && src[1] == '.') {
        // Handle '..'. Back up if possible.
        if (component_count > 0) {
          while (--dst > dst0 && !IsPathSeparator(dst[-1])) {
            // nothing to do here, decrement happens before condition check.
          }
        } else {
          dst[0] = '.';
          dst[1] = '.';
          dst += 2;
          // No separator to add here.
        }
        break;
      }
    }
    // Skip or copy last component, no trailing separator.
    if (dst != src) {
      ::memmove(dst, src, component_len);
    }
    dst += component_len;
  } while (0);

  // Remove trailing path separator if any, but keep the initial
  // path separator(s) if there was one (or two on Windows).
  if (dst > dst_start && IsPathSeparator(dst[-1]))
    dst--;

  if (dst == start) {
    // Handle special cases like "aa/.." -> "."
    *dst++ = '.';
  }

  *len = dst - start;  // dst points after the trailing char here.
#ifdef _WIN32
  uint64_t bits = 0;
  uint64_t bits_mask = 1;

  for (char* c = start; c < start + *len; ++c) {
    switch (*c) {
      case '\\':
        bits |= bits_mask;
        *c = '/';
        NINJA_FALLTHROUGH;
      case '/':
        bits_mask <<= 1;
    }
  }

  // UNC paths enter as "\\server\share\...". After the loop they look like
  // "//server/share/...". FindFirstFileExA treats that form as a network path
  // and fails; restore the Win32 UNC prefix only when those separators were
  // originally backslashes (#2800). Leave an already-forward "//..." alone.
  if (*len >= 2 && start[0] == '/' && start[1] == '/' && (bits & 0x3) == 0x3) {
    start[0] = '\\';
    start[1] = '\\';
  }

  *slash_bits = bits;
#else
  *slash_bits = 0;
#endif
}

static inline bool IsKnownShellSafeCharacter(char ch) {
  if ('A' <= ch && ch <= 'Z') return true;
  if ('a' <= ch && ch <= 'z') return true;
  if ('0' <= ch && ch <= '9') return true;

  switch (ch) {
    case '_':
    case '+':
    case '-':
    case '.':
    case '/':
      return true;
    default:
      return false;
  }
}

static inline bool IsKnownWin32SafeCharacter(char ch) {
  switch (ch) {
    case ' ':
    case '"':
      return false;
    default:
      return true;
  }
}

static inline bool StringNeedsShellEscaping(const std::string& input) {
  for (size_t i = 0; i < input.size(); ++i) {
    if (!IsKnownShellSafeCharacter(input[i])) return true;
  }
  return false;
}

static inline bool StringNeedsWin32Escaping(const std::string& input) {
  for (size_t i = 0; i < input.size(); ++i) {
    if (!IsKnownWin32SafeCharacter(input[i])) return true;
  }
  return false;
}

void GetShellEscapedString(const std::string& input, std::string* result) {
  assert(result);

  if (!StringNeedsShellEscaping(input)) {
    result->append(input);
    return;
  }

  const char kQuote = '\'';
  const char kEscapeSequence[] = "'\\'";

  result->push_back(kQuote);

  std::string::const_iterator span_begin = input.begin();
  for (std::string::const_iterator it = input.begin(), end = input.end();
       it != end; ++it) {
    if (*it == kQuote) {
      result->append(span_begin, it);
      result->append(kEscapeSequence);
      span_begin = it;
    }
  }
  result->append(span_begin, input.end());
  result->push_back(kQuote);
}

void GetWin32EscapedString(const std::string& input, std::string* result) {
  assert(result);
  if (!StringNeedsWin32Escaping(input)) {
    result->append(input);
    return;
  }

  const char kQuote = '"';
  const char kBackslash = '\\';

  result->push_back(kQuote);
  size_t consecutive_backslash_count = 0;
  std::string::const_iterator span_begin = input.begin();
  for (std::string::const_iterator it = input.begin(), end = input.end();
       it != end; ++it) {
    switch (*it) {
      case kBackslash:
        ++consecutive_backslash_count;
        break;
      case kQuote:
        result->append(span_begin, it);
        result->append(consecutive_backslash_count + 1, kBackslash);
        span_begin = it;
        consecutive_backslash_count = 0;
        break;
      default:
        consecutive_backslash_count = 0;
        break;
    }
  }
  result->append(span_begin, input.end());
  result->append(consecutive_backslash_count, kBackslash);
  result->push_back(kQuote);
}

#ifdef _WIN32
std::string GetLastErrorString() {
  DWORD error = GetLastError();
  if (error == 0) {
    return std::string();
  }

  LPSTR message_buffer = nullptr;
  size_t size = FormatMessageA(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
      (LPSTR)&message_buffer, 0, nullptr);

  std::string message(message_buffer, size);
  LocalFree(message_buffer);

  // Remove trailing newlines/period.
  while (!message.empty() && (message.back() == '\r' || message.back() == '\n'))
    message.pop_back();
  while (!message.empty() && message.back() == '.')
    message.pop_back();

  return message;
}
#endif

int ReadFile(const std::string& path, std::string* contents, std::string* err) {
#ifdef _WIN32
  // This makes a ninja run on a set of 1500 manifest files about 4% faster
  // than using the generic fopen code below.
  err->clear();
  HANDLE f = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (f == INVALID_HANDLE_VALUE) {
    err->assign(GetLastErrorString());
    return -ENOENT;
  }

  for (;;) {
    DWORD len;
    char buf[64 << 10];
    if (!::ReadFile(f, buf, sizeof(buf), &len, NULL)) {
      err->assign(GetLastErrorString());
      contents->clear();
      ::CloseHandle(f);
      return -EIO;
    }
    if (len == 0)
      break;
    contents->append(buf, len);
  }
  ::CloseHandle(f);
  return 0;
#else
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    err->assign(strerror(errno));
    return -errno;
  }

  char buf[64 << 10];
  size_t len;
  while (!feof(f) && (len = fread(buf, 1, sizeof(buf), f)) > 0) {
    contents->append(buf, len);
  }
  if (ferror(f)) {
    err->assign(strerror(errno));
    contents->clear();
    fclose(f);
    return -errno;
  }
  fclose(f);
  return 0;
#endif
}


void SetCloseOnExec(int fd) {
  // Windows does not inherit handles unless explicitly asked to, so there is
  // nothing to do here.
  (void)fd;
}

bool AtomicReplace(const string& file_dst, const string& tmp_dst, string* err) {
  if (platformAwareUnlink(file_dst.c_str()) < 0 && errno != ENOENT) {
    *err = strerror(errno);
    return false;
  }
  if (rename(tmp_dst.c_str(), file_dst.c_str()) < 0) {
    *err = strerror(errno);
    return false;
  }
  return true;
}

bool Truncate(const string& path, size_t size, string* err) {
#ifdef _WIN32
  int fh = _sopen(path.c_str(), _O_RDWR | _O_CREAT, _SH_DENYNO,
                  _S_IREAD | _S_IWRITE);
  if (fh < 0) {
    *err = strerror(errno);
    return false;
  }
  int success = _chsize(fh, static_cast<long>(size));
  _close(fh);
#else
  int success = truncate(path.c_str(), size);
#endif
  if (success < 0) {
    *err = strerror(errno);
    return false;
  }
  return true;
}

int platformAwareUnlink(const char* filename) {
#ifdef _WIN32
  return _unlink(filename);
#else
  return unlink(filename);
#endif
}

#ifdef _WIN32
bool GetHybridCoreCounts(int* pcores, int* ecores) {
  *pcores = 0;
  *ecores = 0;

  ULONG len = 0;
  if (!GetSystemCpuSetInformation(nullptr, 0, &len, nullptr, 0) || len == 0)
    return false;
  std::vector<BYTE> buffer(len);
  if (!GetSystemCpuSetInformation(
          reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()), len,
          &len, nullptr, 0))
    return false;

  // SMT siblings share a (group, core index) pair, so count each physical core
  // once and remember which class it was.
  std::set<std::pair<WORD, BYTE>> seen;
  ULONG offset = 0;
  while (offset + sizeof(DWORD) <= len) {
    const auto* info =
        reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() +
                                                           offset);
    if (info->Size == 0)
      break;
    if (info->Type == CpuSetInformation &&
        (info->CpuSet.AllFlags & SYSTEM_CPU_SET_INFORMATION_PARKED) == 0 &&
        (info->CpuSet.AllFlags & SYSTEM_CPU_SET_INFORMATION_ALLOCATED) != 0) {
      auto key = std::make_pair(info->CpuSet.Group, info->CpuSet.CoreIndex);
      if (seen.insert(key).second) {
        if (info->CpuSet.EfficiencyClass == 0)
          ++*pcores;
        else
          ++*ecores;
      }
    }
    offset += info->Size;
  }
  return *pcores > 0 || *ecores > 0;
}
#endif
void MakeNonInheritable(FILE* fp) {
#ifdef _WIN32
  // MSVCRT's fopen() does not use O_NOINHERIT, so a descriptor opened here would
  // be inherited by every child process we spawn with bInheritHandles = TRUE.
  // That matters for files we hold open across a build (.ninja_log,
  // .ninja_deps, .ninja_lock): a child that inherited one keeps it locked even
  // after we exit, which breaks other tools (and our own next run).
  int fd = _fileno(fp);
  if (fd < 0)
    return;
  intptr_t handle = _get_osfhandle(fd);
  if (handle == -1 || handle == -2)
    return;
  SetHandleInformation(reinterpret_cast<HANDLE>(handle), HANDLE_FLAG_INHERIT, 0);
#else
  (void)fp;
#endif
}
int GetProcessHandleCount() {
#ifdef _WIN32
  DWORD count = 0;
  if (!GetProcessHandleCount(GetCurrentProcess(), &count))
    return -1;
  return static_cast<int>(count);
#else
  return -1;
#endif
}

int GetProcessorCount() {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  int count = (int)info.dwNumberOfProcessors;
  return count > 0 ? count : 1;
}

int GetDefaultParallelism() {
#ifdef _WIN32
  // On a hybrid CPU (P cores plus E cores) the efficiency cores are much slower
  // per clock, and oversubscribing them with build jobs makes the whole build
  // slower rather than faster.  Prefer the performance cores.
  int pcores = 0, ecores = 0;
  if (GetHybridCoreCounts(&pcores, &ecores) && pcores > 0)
    return pcores;
#endif
  return GetProcessorCount();
}



void SetupMsvcEnvironment() {
#ifdef _WIN32
  // Running vcvars64.bat costs about a second, so do it only when it is
  // actually needed:
  //   * if the environment already looks like a developer prompt (LIB points at
  //     an MSVC tools directory), there is nothing to do;
  //   * otherwise it is worth it, because without LIB/INCLUDE the compiler and
  //     linker cannot find the CRT or the Windows SDK.
  if (const char* lib = getenv("LIB")) {
    if (strstr(lib, "Tools\\MSVC") || strstr(lib, "Tools/MSVC"))
      return;
  }

  // Prefer the VS 2022 install the user pointed us at; fall back to vswhere.
  const char* kCandidateRoots[] = { "D:\\vs2022", "D:\\vs2025", "D:\\vs2026" };
  std::string install_path;
  for (const char* root : kCandidateRoots) {
    char vcvars[2048];
    snprintf(vcvars, sizeof(vcvars), "%s\\VC\\Auxiliary\\Build\\vcvars64.bat", root);
    if (GetFileAttributesA(vcvars) != INVALID_FILE_ATTRIBUTES) {
      install_path = root;
      break;
    }
  }
  if (install_path.empty()) {
    const char* kVswhere =
        "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
    if (GetFileAttributesA(kVswhere) == INVALID_FILE_ATTRIBUTES)
      return;
    char query[512];
    snprintf(query, sizeof(query),
             "\"%s\" -latest -products * -property installationPath", kVswhere);
    FILE* pipe = _popen(query, "r");
    if (!pipe)
      return;
    char path[1024] = {0};
    if (!fgets(path, sizeof(path), pipe)) {
      _pclose(pipe);
      return;
    }
    _pclose(pipe);
    size_t len = strlen(path);
    while (len > 0 && (path[len - 1] == '\n' || path[len - 1] == '\r'))
      path[--len] = 0;
    if (len == 0)
      return;
    install_path = path;
  }

  char vcvars[2048];
  snprintf(vcvars, sizeof(vcvars), "%s\\VC\\Auxiliary\\Build\\vcvars64.bat",
           install_path.c_str());
  // Dump the post-vcvars environment through a temporary batch file: nesting
  // quotes inside _popen("cmd /c ...") is fragile.
  char tmp_bat[MAX_PATH], tmp_out[MAX_PATH];
  GetTempPathA(MAX_PATH, tmp_bat);
  GetTempPathA(MAX_PATH, tmp_out);
  snprintf(tmp_bat + strlen(tmp_bat), sizeof(tmp_bat) - strlen(tmp_bat),
           "sninja_env_%lu.bat", GetCurrentProcessId());
  snprintf(tmp_out + strlen(tmp_out), sizeof(tmp_out) - strlen(tmp_out),
           "sninja_env_%lu.txt", GetCurrentProcessId());
  FILE* bat = fopen(tmp_bat, "wb");
  if (!bat)
    return;
  fprintf(bat, "@echo off\r\ncall \"%s\" >nul 2>&1\r\nset > \"%s\"\r\n",
          vcvars, tmp_out);
  fclose(bat);

  char cmd[4096];
  snprintf(cmd, sizeof(cmd), "cmd /c \"%s\"", tmp_bat);
  FILE* pipe = _popen(cmd, "r");
  if (pipe)
    _pclose(pipe);

  FILE* env_file = fopen(tmp_out, "rb");
  if (!env_file) {
    DeleteFileA(tmp_bat);
    return;
  }
  char line[8192];
  while (fgets(line, sizeof(line), env_file)) {
    char* eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = 0;
    char* value = eq + 1;
    size_t vlen = strlen(value);
    while (vlen > 0 &&
           (value[vlen - 1] == '\n' || value[vlen - 1] == '\r'))
      value[--vlen] = 0;
    if (strcmp(line, "LIB") == 0 || strcmp(line, "INCLUDE") == 0 ||
        strcmp(line, "PATH") == 0 || strcmp(line, "LIBPATH") == 0) {
      SetEnvironmentVariableA(line, value);
    }
  }
  fclose(env_file);
  DeleteFileA(tmp_bat);
  DeleteFileA(tmp_out);
#endif
}
int64_t NowMillis() {
  return static_cast<int64_t>(GetTickCount64());
}

string GetWorkingDirectory() {
  string ret;
  char* success = NULL;
  do {
    ret.resize(ret.size() + 1024);
    errno = 0;
    #ifdef _WIN32
    success = _getcwd(&ret[0], static_cast<int>(ret.size()));
#else
    success = getcwd(&ret[0], static_cast<int>(ret.size()));
#endif
  } while (!success && errno == ERANGE);
  if (!success)
    Fatal("cannot determine working directory: %s", strerror(errno));
  ret.resize(strlen(&ret[0]));
  return ret;
}
