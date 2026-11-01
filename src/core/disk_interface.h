#ifndef SNINJA_DISK_INTERFACE_H_
#define SNINJA_DISK_INTERFACE_H_

#include <map>
#include <string>

#include "timestamp.h"

/// Interface for reading files from disk.  See DiskInterface for details.
/// This base offers the minimum interface needed just to read files.
struct FileReader {
  virtual ~FileReader() {}

  /// Result of ReadFile.
  enum Status {
    Okay,
    NotFound,
    OtherError
  };

  /// Read and store in given string.  On success, return Okay.
  /// On error, return another Status and fill |err|.
  virtual Status ReadFile(const std::string& path, std::string* contents,
                          std::string* err) = 0;
};

/// Interface for accessing the disk.
///
/// Abstract so it can be mocked out for tests.  The real implementation
/// is RealDiskInterface.
struct DiskInterface : public FileReader {
  /// stat() a file, returning the mtime, or 0 if missing and -1 on
  /// other errors.
  virtual TimeStamp Stat(const std::string& path, std::string* err) const = 0;

  /// Create a directory, returning false on failure.
  virtual bool MakeDir(const std::string& path) = 0;

  /// Create a file, with the specified name and contents.
  virtual bool WriteFile(const std::string& path, const std::string& contents,
                         bool crlf_on_windows) = 0;

  /// Remove the file named @a path. It behaves like 'rm -f path' so no errors
  /// are reported if it does not exists.
  /// @returns 0 if the file has been removed,
  ///          1 if the file does not exist, and
  ///          -1 if an error occurs.
  virtual int RemoveFile(const std::string& path) = 0;

  /// Create all the parent directories for path; like mkdir -p
  /// `basename path`.
  bool MakeDirs(const std::string& path);
};

/// Implementation of DiskInterface that actually hits the disk.
struct RealDiskInterface : public DiskInterface {
  RealDiskInterface();
  ~RealDiskInterface() override {}
  TimeStamp Stat(const std::string& path, std::string* err) const override;
  bool MakeDir(const std::string& path) override;
  bool WriteFile(const std::string& path, const std::string& contents,
                 bool crlf_on_windows) override;
  Status ReadFile(const std::string& path, std::string* contents,
                  std::string* err) override;
  int RemoveFile(const std::string& path) override;

  /// Whether stat information can be cached.  Only has an effect on Windows.
  void AllowStatCache(bool allow);
};

#endif  // SNINJA_DISK_INTERFACE_H_
