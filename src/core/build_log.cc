#include "build_log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include <cassert>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "disk_interface.h"
#include "graph.h"
#include "third_party/rapidhash/rapidhash.h"
#include "util.h"

using namespace std;

// Implementation details:
// Each run's log appends to the log file.
// To load, we run through all log entries in series, throwing away
// older runs.
// Once the number of redundant entries exceeds a threshold, we write
// out a new file and replace the existing one with it.

namespace {

const char kFileSignature[] = "# ninja log v%d\n";

#ifdef _WIN32
int Fileno(FILE* f) { return _fileno(f); }
#else
int Fileno(FILE* f) { return fileno(f); }
#endif
const int kOldestSupportedVersion = 7;
const int kCurrentVersion = 7;

}  // namespace

// static
uint64_t BuildLog::LogEntry::HashCommand(std::string_view command) {
  return rapidhash(command.data(), command.size());
}

BuildLog::LogEntry::LogEntry(std::string output) : output(std::move(output)) {}

BuildLog::LogEntry::LogEntry(const std::string& output, uint64_t command_hash,
                             int start_time, int end_time, TimeStamp mtime)
    : output(output), command_hash(command_hash), start_time(start_time),
      end_time(end_time), mtime(mtime) {}

BuildLog::BuildLog() = default;

BuildLog::~BuildLog() {
  Close();
}

bool BuildLog::OpenForWrite(const std::string& path, const BuildLogUser& user,
                            std::string* err) {
  if (needs_recompaction_) {
    if (!Recompact(path, user, err))
      return false;
  }

  assert(!log_file_);
  // We don't actually open the file right now, but will do so on the first
  // write attempt.
  log_file_path_ = path;
  return true;
}

bool BuildLog::RecordCommand(Edge* edge, int start_time, int end_time,
                             TimeStamp mtime) {
  std::string command = edge->EvaluateCommand(true);
  uint64_t command_hash = LogEntry::HashCommand(command);
  for (std::vector<Node*>::iterator out = edge->outputs_.begin();
       out != edge->outputs_.end(); ++out) {
    const std::string& path = (*out)->path();
    std::unique_ptr<LogEntry>* slot = entries_.find(path);
  LogEntry* log_entry = slot ? slot->get() : nullptr;
    if (!log_entry) {
      auto new_entry = std::make_unique<LogEntry>(path);
      log_entry = new_entry.get();
      // The key is a view into the entry's own |output|, which the map keeps
      // alive through the unique_ptr.
      entries_.insert(log_entry->output, std::move(new_entry));
    }
    log_entry->command_hash = command_hash;
    log_entry->start_time = start_time;
    log_entry->end_time = end_time;
    log_entry->mtime = mtime;

    if (!OpenForWriteIfNeeded()) {
      return false;
    }
    if (log_file_) {
      if (!WriteEntry(log_file_, *log_entry))
        return false;
      if (fflush(log_file_) != 0) {
        return false;
      }
    }
  }
  return true;
}

void BuildLog::Close() {
  OpenForWriteIfNeeded();  // create the file even if nothing has been recorded
  if (log_file_)
    fclose(log_file_);
  log_file_ = NULL;
}

bool BuildLog::OpenForWriteIfNeeded() {
  if (log_file_ || log_file_path_.empty()) {
    return true;
  }
  log_file_ = fopen(log_file_path_.c_str(), "ab");
  if (!log_file_) {
    return false;
  }
  if (setvbuf(log_file_, NULL, _IOLBF, BUFSIZ) != 0) {
    return false;
  }
  SetCloseOnExec(Fileno(log_file_));

  // Opening a file in append mode doesn't set the file pointer to the file's
  // end on Windows. Do that explicitly.
  fseek(log_file_, 0, SEEK_END);

  if (ftell(log_file_) == 0) {
    if (fprintf(log_file_, kFileSignature, kCurrentVersion) < 0) {
      return false;
    }
  }
  return true;
}

namespace {

struct LineReader {
  explicit LineReader(FILE* file)
      : file_(file), buf_end_(buf_), line_start_(buf_), line_end_(NULL) {
    memset(buf_, 0, sizeof(buf_));
  }

  // Reads a \n-terminated line from the file passed to the constructor.
  // On return, *line_start points to the beginning of the next line, and
  // *line_end points to the \n at the end of the line. If no newline is seen
  // in a fixed buffer size, *line_end is set to NULL. Returns false on EOF.
  bool ReadLine(char** line_start, char** line_end) {
    if (line_start_ >= buf_end_ || !line_end_) {
      // Buffer empty, refill.
      size_t size_read = fread(buf_, 1, sizeof(buf_), file_);
      if (!size_read)
        return false;
      line_start_ = buf_;
      buf_end_ = buf_ + size_read;
    } else {
      // Advance to next line in buffer.
      line_start_ = line_end_ + 1;
    }

    line_end_ =
        static_cast<char*>(memchr(line_start_, '\n', buf_end_ - line_start_));
    if (!line_end_) {
      // No newline. Move rest of data to start of buffer, fill rest.
      size_t already_consumed = line_start_ - buf_;
      size_t size_rest = (buf_end_ - buf_) - already_consumed;
      memmove(buf_, line_start_, size_rest);

      size_t read = fread(buf_ + size_rest, 1, sizeof(buf_) - size_rest, file_);
      buf_end_ = buf_ + size_rest + read;
      line_start_ = buf_;
      line_end_ =
          static_cast<char*>(memchr(line_start_, '\n', buf_end_ - line_start_));
    }

    *line_start = line_start_;
    *line_end = line_end_;
    return true;
  }

 private:
  FILE* file_;
  char buf_[256 << 10];
  char* buf_end_;  // Points one past the last valid byte in |buf_|.

  char* line_start_;
  // Points at the next \n in buf_ after line_start, or NULL.
  char* line_end_;
};

}  // namespace

LoadStatus BuildLog::Load(const std::string& path, std::string* err) {
  FILE* file = fopen(path.c_str(), "r");
  if (!file) {
    if (errno == ENOENT)
      return LOAD_NOT_FOUND;
    *err = strerror(errno);
    return LOAD_ERROR;
  }

  int log_version = 0;
  int unique_entry_count = 0;
  int total_entry_count = 0;

  LineReader reader(file);
  char* line_start = 0;
  char* line_end = 0;
  while (reader.ReadLine(&line_start, &line_end)) {
    if (!log_version) {
      sscanf(line_start, kFileSignature, &log_version);

      bool invalid_log_version = false;
      if (log_version < kOldestSupportedVersion) {
        invalid_log_version = true;
        *err = "build log version is too old; starting over";
      } else if (log_version > kCurrentVersion) {
        invalid_log_version = true;
        *err = "build log version is too new; starting over";
      }
      if (invalid_log_version) {
        fclose(file);
        platformAwareUnlink(path.c_str());
        // Don't report this as a failure.  A missing build log will cause us to
        // rebuild the outputs anyway.
        return LOAD_NOT_FOUND;
      }
    }

    // If no newline was found in this chunk, read the next.
    if (!line_end)
      continue;

    const char kFieldSeparator = '\t';

    char* start = line_start;
    char* end =
        static_cast<char*>(memchr(start, kFieldSeparator, line_end - start));
    if (!end)
      continue;
    *end = 0;

    int start_time = 0, end_time = 0;
    TimeStamp mtime = 0;

    start_time = atoi(start);
    start = end + 1;

    end = static_cast<char*>(memchr(start, kFieldSeparator, line_end - start));
    if (!end)
      continue;
    *end = 0;
    end_time = atoi(start);
    start = end + 1;

    end = static_cast<char*>(memchr(start, kFieldSeparator, line_end - start));
    if (!end)
      continue;
    *end = 0;
    mtime = strtoll(start, NULL, 10);
    start = end + 1;

    end = static_cast<char*>(memchr(start, kFieldSeparator, line_end - start));
    if (!end)
      continue;
    std::string output(start, end - start);

    start = end + 1;
    end = line_end;

    std::unique_ptr<LogEntry>* slot = entries_.find(output);
    LogEntry* entry = slot ? slot->get() : nullptr;
    if (!entry) {
      auto new_entry = std::make_unique<LogEntry>(std::move(output));
      entry = new_entry.get();
      entries_.insert(entry->output, std::move(new_entry));
      ++unique_entry_count;
    }
    ++total_entry_count;

    entry->start_time = start_time;
    entry->end_time = end_time;
    entry->mtime = mtime;
    char c = *end;
    *end = '\0';
    entry->command_hash = (uint64_t)strtoull(start, NULL, 16);
    *end = c;
  }
  fclose(file);

  if (!line_start) {
    return LOAD_SUCCESS;  // file was empty
  }

  // Decide whether it's time to rebuild the log:
  // - if we're upgrading versions
  // - if it's getting large
  int kMinCompactionEntryCount = 100;
  int kCompactionRatio = 3;
  if (log_version < kCurrentVersion) {
    needs_recompaction_ = true;
  } else if (total_entry_count > kMinCompactionEntryCount &&
             total_entry_count > unique_entry_count * kCompactionRatio) {
    needs_recompaction_ = true;
  }

  return LOAD_SUCCESS;
}

BuildLog::LogEntry* BuildLog::LookupByOutput(const std::string& path) const {
  std::unique_ptr<LogEntry>* slot = entries_.find(path);
  return slot ? slot->get() : NULL;
}

bool BuildLog::WriteEntry(FILE* f, const LogEntry& entry) {
  return fprintf(f, "%d\t%d\t%" PRId64 "\t%s\t%" PRIx64 "\n", entry.start_time,
                 entry.end_time, entry.mtime, entry.output.c_str(),
                 entry.command_hash) > 0;
}


bool BuildLog::Recompact(const std::string& path, const BuildLogUser& user,
                         std::string* err) {
  Close();
  std::string temp_path = path + ".recompact";
  FILE* f = fopen(temp_path.c_str(), "wb");
  if (!f) {
    *err = strerror(errno);
    return false;
  }

  if (fprintf(f, kFileSignature, kCurrentVersion) < 0) {
    *err = strerror(errno);
    fclose(f);
    return false;
  }

  std::vector<std::string_view> dead_outputs;
  for (const auto& pair : entries_) {
    if (user.IsPathDead(pair.first)) {
      dead_outputs.push_back(pair.first);
      continue;
    }

    if (!WriteEntry(f, *pair.second)) {
      *err = strerror(errno);
      fclose(f);
      return false;
    }
  }

  fclose(f);

  return AtomicReplace(path, temp_path, err);
}

bool BuildLog::Restat(const std::string& path, const DiskInterface& disk_interface,
                      const int output_count, char** outputs,
                      std::string* const err) {
  Close();
  std::string temp_path = path + ".restat";
  FILE* f = fopen(temp_path.c_str(), "wb");
  if (!f) {
    *err = strerror(errno);
    return false;
  }

  if (fprintf(f, kFileSignature, kCurrentVersion) < 0) {
    *err = strerror(errno);
    fclose(f);
    return false;
  }
  for (const auto& pair : entries_) {
    bool skip = output_count > 0;
    for (int j = 0; j < output_count; ++j) {
      if (pair.second->output == outputs[j]) {
        skip = false;
        break;
      }
    }
    if (!skip) {
      const TimeStamp mtime = disk_interface.Stat(pair.second->output, err);
      if (mtime == -1) {
        fclose(f);
        return false;
      }
      pair.second->mtime = mtime;
    }

    if (!WriteEntry(f, *pair.second)) {
      *err = strerror(errno);
      fclose(f);
      return false;
    }
  }

  fclose(f);

  return AtomicReplace(path, temp_path, err);
}
