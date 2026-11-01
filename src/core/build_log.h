#ifndef SNINJA_BUILD_LOG_H_
#define SNINJA_BUILD_LOG_H_

#include <stdio.h>

#include <memory>
#include <string>
#include <string_view>

#include "hash.h"
#include "load_status.h"
#include "timestamp.h"

struct DiskInterface;
struct Edge;

/// Can answer questions about the manifest for the BuildLog.
struct BuildLogUser {
  /// Return if a given output is no longer part of the build manifest.
  /// This is only called during recompaction and doesn't have to be fast.
  virtual bool IsPathDead(std::string_view s) const = 0;
};

/// Store a log of every command ran for every build.
/// It has a few uses:
///
/// 1) (hashes of) command lines for existing output files, so we know
///    when we need to rebuild due to the command changing
/// 2) timing information, perhaps for generating reports
/// 3) restat information
///
/// The on-disk format is byte-compatible with upstream ninja's .ninja_log.
struct BuildLog {
  BuildLog();
  ~BuildLog();

  /// Prepares writing to the log file without actually opening it - that will
  /// happen when/if it's needed
  bool OpenForWrite(const std::string& path, const BuildLogUser& user,
                    std::string* err);
  bool RecordCommand(Edge* edge, int start_time, int end_time,
                     TimeStamp mtime = 0);
  void Close();

  /// Load the on-disk log.
  LoadStatus Load(const std::string& path, std::string* err);

  struct LogEntry {
    std::string output;
    uint64_t command_hash = 0;
    int start_time = 0;
    int end_time = 0;
    TimeStamp mtime = 0;

    /// rapidhash of the command line, matching upstream ninja bit for bit.
    static uint64_t HashCommand(std::string_view command);

    // Used by tests.
    bool operator==(const LogEntry& o) const {
      return output == o.output && command_hash == o.command_hash &&
             start_time == o.start_time && end_time == o.end_time &&
             mtime == o.mtime;
    }

    explicit LogEntry(std::string output);
    LogEntry(const std::string& output, uint64_t command_hash, int start_time,
             int end_time, TimeStamp mtime);
  };

  /// Lookup a previously-run command by its output path.
  LogEntry* LookupByOutput(const std::string& path) const;

  /// Serialize an entry into a log file.
  bool WriteEntry(FILE* f, const LogEntry& entry);

  /// Rewrite the known log entries, throwing away old data.
  bool Recompact(const std::string& path, const BuildLogUser& user,
                 std::string* err);

  /// Restat all outputs in the log.
  bool Restat(const std::string& path, const DiskInterface& disk_interface,
              int output_count, char** outputs, std::string* err);

  /// Keys are string_views into LogEntry::output, which outlives the map entry.
  typedef sninja::StrViewMap<std::unique_ptr<LogEntry>> Entries;
  const Entries& entries() const { return entries_; }

 private:
  /// Should be called before using log_file_. When false is returned, errno
  /// will be set.
  bool OpenForWriteIfNeeded();

  Entries entries_;
  FILE* log_file_ = nullptr;
  std::string log_file_path_;
  bool needs_recompaction_ = false;
};

#endif  // SNINJA_BUILD_LOG_H_
