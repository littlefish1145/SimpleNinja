#include "disk_interface.h"

#include <algorithm>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>  // _mkdir
#include <windows.h>

#include <sstream>
#else
#include <unistd.h>
#endif

#include "util.h"

using namespace std;

namespace {

string DirName(const string& path) {
#ifdef _WIN32
  static const char kPathSeparators[] = "\\/";
#else
  static const char kPathSeparators[] = "/";
#endif
  static const char* const kEnd = kPathSeparators + sizeof(kPathSeparators) - 1;

  string::size_type slash_pos = path.find_last_of(kPathSeparators);
  if (slash_pos == string::npos)
    return string();  // Nothing to do.
  while (slash_pos > 0 &&
         std::find(kPathSeparators, kEnd, path[slash_pos - 1]) != kEnd)
    --slash_pos;
  return path.substr(0, slash_pos);
}

int MakeDir(const string& path) {
#ifdef _WIN32
  return _mkdir(path.c_str());
#else
  return mkdir(path.c_str(), 0777);
#endif
}

#ifdef _WIN32
TimeStamp TimeStampFromFileTime(const FILETIME& filetime) {
  // FILETIME is in 100-nanosecond increments since the Windows epoch.
  // We don't much care about epoch correctness but we do want the
  // resulting value to fit in a 64-bit integer.
  uint64_t mtime = ((uint64_t)filetime.dwHighDateTime << 32) |
    ((uint64_t)filetime.dwLowDateTime);
  // 1600 epoch -> 2000 epoch (subtract 400 years).
  return (TimeStamp)mtime - 12622770400LL * (1000000000LL / 100);
}

TimeStamp StatSingleFile(const string& path, string* err) {
  WIN32_FILE_ATTRIBUTE_DATA attrs;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs)) {
    DWORD win_err = GetLastError();
    if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND)
      return 0;
    *err = "GetFileAttributesEx(" + path + "): " + GetLastErrorString();
    return -1;
  }

  if (attrs.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    HANDLE hFile = CreateFileA(path.c_str(), 0, 0, 0, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (hFile == INVALID_HANDLE_VALUE) {
      DWORD win_err = GetLastError();
      if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND)
        return 0;
      *err = "CreateFileA(" + path + "): " + GetLastErrorString();
      CloseHandle(hFile);
      return -1;
    }

    CHAR pathBuf[MAX_PATH];
    if (GetFinalPathNameByHandleA(hFile, pathBuf, MAX_PATH,
                                  FILE_NAME_NORMALIZED) == 0) {
      DWORD win_err = GetLastError();
      if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND)
        return 0;
      *err = "GetFinalPathNameByHandleA(" + path + "): " + GetLastErrorString();
      CloseHandle(hFile);
      return -1;
    }
    CloseHandle(hFile);
    return StatSingleFile(pathBuf, err);
  }

  return TimeStampFromFileTime(attrs.ftLastWriteTime);
}
#endif  // _WIN32

}  // namespace

// DiskInterface ---------------------------------------------------------------

bool DiskInterface::MakeDirs(const string& path) {
  string dir = DirName(path);
  if (dir.empty())
    return true;  // Reached root; assume it's there.
  string err;
  TimeStamp mtime = Stat(dir, &err);
  if (mtime < 0) {
    Error("%s", err.c_str());
    return false;
  }
  if (mtime > 0)
    return true;  // Exists already; we're done.

  // Directory doesn't exist.  Try creating its parent first.
  bool success = MakeDirs(dir);
  if (!success)
    return false;
  return MakeDir(dir);
}

// RealDiskInterface -----------------------------------------------------------
RealDiskInterface::RealDiskInterface() {}

TimeStamp RealDiskInterface::Stat(const string& path, string* err) const {
#ifdef _WIN32
  if (!path.empty() && path.size() > MAX_PATH) {
    ostringstream err_stream;
    err_stream << "Stat(" << path << "): Filename longer than " << MAX_PATH
               << " characters";
    *err = err_stream.str();
    return -1;
  }
  return StatSingleFile(path, err);
#else
  struct stat st;
  if (stat(path.c_str(), &st) < 0) {
    if (errno == ENOENT || errno == ENOTDIR)
      return 0;
    *err = "stat(" + path + "): " + strerror(errno);
    return -1;
  }
  // Some users (Flatpak) set mtime to 0, this should be harmless
  // and avoids conflicting with our return value of 0 meaning
  // that it doesn't exist.
  if (st.st_mtime == 0)
    return 1;
#if defined(__APPLE__)
  return ((int64_t)st.st_mtimespec.tv_sec * 1000000000LL +
          st.st_mtimespec.tv_nsec);
#else
  return (int64_t)st.st_mtime * 1000000000LL + st.st_mtim.tv_nsec;
#endif
#endif
}

bool RealDiskInterface::WriteFile(const string& path, const string& contents,
                                  bool crlf_on_windows) {
  FILE* fp = fopen(path.c_str(),
#ifdef _WIN32
                   crlf_on_windows ? "w" : "wb");
#else
                   "wb");
  (void)crlf_on_windows;
#endif
  if (fp == NULL) {
    Error("WriteFile(%s): Unable to create file. %s",
          path.c_str(), strerror(errno));
    return false;
  }

  if (fwrite(contents.data(), 1, contents.length(), fp) < contents.length()) {
    Error("WriteFile(%s): Unable to write to the file. %s",
          path.c_str(), strerror(errno));
    fclose(fp);
    return false;
  }

  MakeNonInheritable(fp);

  if (fclose(fp) == EOF) {
    Error("WriteFile(%s): Unable to close the file. %s",
          path.c_str(), strerror(errno));
    return false;
  }

  return true;
}

bool RealDiskInterface::MakeDir(const string& path) {
  if (::MakeDir(path) < 0) {
    if (errno == EEXIST) {
      return true;
    }
    Error("mkdir(%s): %s", path.c_str(), strerror(errno));
    return false;
  }
  return true;
}

FileReader::Status RealDiskInterface::ReadFile(const string& path,
                                               string* contents,
                                               string* err) {
  switch (::ReadFile(path, contents, err)) {
    case 0:       return Okay;
    case -ENOENT: return NotFound;
    default:      return OtherError;
  }
}

int RealDiskInterface::RemoveFile(const string& path) {
#ifdef _WIN32
  DWORD attributes = GetFileAttributesA(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    DWORD win_err = GetLastError();
    if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND) {
      return 1;
    }
  } else if (attributes & FILE_ATTRIBUTE_READONLY) {
    // On non-Windows systems, remove() will happily delete read-only files.
    // On Windows Ninja should behave the same.
    SetFileAttributesA(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
  }
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
    // remove() deletes both files and directories. On Windows we have to
    // select the correct function (DeleteFile will yield Permission Denied
    // when used on a directory).
    if (!RemoveDirectoryA(path.c_str())) {
      DWORD win_err = GetLastError();
      if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND) {
        return 1;
      }
      // Report remove(), not RemoveDirectory(), for cross-platform consistency.
      Error("remove(%s): %s", path.c_str(), GetLastErrorString().c_str());
      return -1;
    }
  } else {
    if (!DeleteFileA(path.c_str())) {
      DWORD win_err = GetLastError();
      if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND) {
        return 1;
      }
      // Report as remove(), not DeleteFile(), for cross-platform consistency.
      Error("remove(%s): %s", path.c_str(), GetLastErrorString().c_str());
      return -1;
    }
  }
#else
  if (remove(path.c_str()) < 0) {
    switch (errno) {
      case ENOENT:
        return 1;
      default:
        Error("remove(%s): %s", path.c_str(), strerror(errno));
        return -1;
    }
  }
#endif
  return 0;
}

void RealDiskInterface::AllowStatCache(bool allow) {
  (void)allow;
}
