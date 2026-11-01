// Copyright 2012 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "deps_log.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#ifndef _WIN32
#include <unistd.h>
#elif defined(_MSC_VER) && (_MSC_VER < 1900)
typedef __int32 int32_t;
typedef unsigned __int32 uint32_t;
#endif

#include "graph.h"
#include "metrics.h"
#include "state.h"
#include "util.h"

using namespace std;

// The version is stored as 4 bytes after the signature and also serves as a
// byte order mark. Signature and version combined are 16 bytes long.
static const char kFileSignature[] = "# ninjadeps\n";
static const size_t kFileSignatureSize = sizeof(kFileSignature) - 1u;

static const int32_t kCurrentVersion = 4;

// Record size is currently limited to less than the full 32 bit, due to
// internal buffers having to have this size.
static constexpr size_t kMaxRecordSize = (1 << 19) - 1;

namespace {

// Read many complete records per stdio call, borrowing each payload until the
// next refill. Read whole blocks from the start of the file so stdio can bypass
// its own small buffer instead of splitting each refill into multiple reads.
// At most one block beyond a record is retained, regardless of the log size.
class DepsRecordReader {
 public:
  explicit DepsRecordReader(FILE* file)
      : file_(file), buffer_(std::make_unique_for_overwrite<char[]>(capacity_)) {}

  const char* Read(size_t size) {
    if (end_ - next_ < size) {
      const size_t remaining = end_ - next_;
      const size_t read_size =
          (size - remaining + kBlockSize - 1) & ~(kBlockSize - 1);
      const size_t required = remaining + read_size;
      if (required > capacity_) {
        const size_t capacity = (required + kBlockSize - 1) & ~(kBlockSize - 1);
        auto replacement = std::make_unique_for_overwrite<char[]>(capacity);
        memcpy(replacement.get(), buffer_.get() + next_, remaining);
        buffer_ = std::move(replacement);
        capacity_ = capacity;
      } else {
        memmove(buffer_.get(), buffer_.get() + next_, remaining);
      }
      next_ = 0;
      end_ = remaining + fread(buffer_.get() + remaining, 1, read_size, file_);
      if (end_ < size)
        return nullptr;
    }
    const char* result = buffer_.get() + next_;
    next_ += size;
    return result;
  }

  size_t ReadInto(void* destination, size_t size) {
    if (const char* data = Read(size)) {
      memcpy(destination, data, size);
      return size;
    }
    // Match fread's partial-element writes too: a truncated legacy version can
    // still change the initialized version word and select its old diagnostic.
    const size_t remaining = end_ - next_;
    memcpy(destination, buffer_.get() + next_, remaining);
    next_ = end_;
    return remaining;
  }

 private:
  static constexpr size_t kBlockSize = 64 << 10;
  FILE* file_;
  size_t capacity_ = kBlockSize;
  std::unique_ptr<char[]> buffer_;
  size_t next_ = 0;
  size_t end_ = 0;
};

// A corrupt path length can leave the next record unaligned. memcpy supports
// that case without violating the C++ object lifetime or alignment rules.
uint32_t ReadWord(const char* data) {
  uint32_t word;
  memcpy(&word, data, sizeof(word));
  return word;
}

}  // namespace

DepsLog::~DepsLog() {
  Close();
  for (Deps* deps : deps_)
    delete deps;
}

bool DepsLog::OpenForWrite(const string& path, string* err) {
  if (needs_recompaction_) {
    if (!Recompact(path, err))
      return false;
  }

  assert(!file_);
  file_path_ = path;  // we don't actually open the file right now, but will do
                      // so on the first write attempt
  return true;
}

bool DepsLog::RecordDeps(Node* node, TimeStamp mtime,
                         const vector<Node*>& nodes) {
  return RecordDeps(node, mtime, static_cast<int>(nodes.size()), nodes.data());
}

bool DepsLog::RecordDeps(Node* node, TimeStamp mtime, int node_count,
                         Node* const* nodes) {
  return RecordDepsImpl<true>(node, mtime, node_count, nodes);
}

template <bool KeepInMemory, class Nodes>
bool DepsLog::RecordDepsImpl(Node* node, TimeStamp mtime, int node_count,
                             const Nodes& nodes) {
  // Track whether there's any new data to be recorded.
  bool made_change = false;

  // Assign ids to all nodes that are missing one.
  if (node->id() < 0) {
    if (!RecordId(node))
      return false;
    made_change = true;
  }
  for (int i = 0; i < node_count; ++i) {
    if (nodes[i]->id() < 0) {
      if (!RecordId(nodes[i]))
        return false;
      made_change = true;
    }
  }

  if constexpr (KeepInMemory) {
    // See if the new data is different than the existing data, if any.
    if (!made_change) {
      Deps* deps = GetDeps(node);
      if (!deps ||
          deps->mtime != mtime ||
          deps->node_count != node_count) {
        made_change = true;
      } else {
        for (int i = 0; i < node_count; ++i) {
          if (deps->nodes[i] != nodes[i]) {
            made_change = true;
            break;
          }
        }
      }
    }

    // Don't write anything if there's no new info.
    if (!made_change)
      return true;
  }

  // Update on-disk representation.
  unsigned size = 4 * (1 + 2 + node_count);
  if (size > kMaxRecordSize) {
    errno = ERANGE;
    return false;
  }

  if (!OpenForWriteIfNeeded()) {
    return false;
  }
  write_buffer_.resize(1 + size / 4);
  write_buffer_[0] = size | 0x80000000;  // Deps record: set high bit.
  write_buffer_[1] = node->id();
  write_buffer_[2] = static_cast<uint32_t>(mtime);
  write_buffer_[3] = static_cast<uint32_t>(uint64_t(mtime) >> 32);
  for (int i = 0; i < node_count; ++i)
    write_buffer_[4 + i] = nodes[i]->id();
  if (fwrite(write_buffer_.data(), size + 4, 1, file_) < 1)
    return false;
  if (flush_on_record_ && fflush(file_) != 0)
    return false;

  // Update in-memory representation.
  if constexpr (KeepInMemory) {
    Deps* deps = Deps::Create(mtime, node_count, &nodes_);
    memcpy(deps->nodes.ids, write_buffer_.data() + 4, sizeof(uint32_t) * node_count);
    UpdateDeps(node->id(), deps);
  }

  return true;
}

void DepsLog::Close() {
  CloseForWrite();
}

bool DepsLog::CloseForWrite() {
  // Create an empty log too, but do not retry a failed open from the destructor.
  const bool opened = file_ || OpenForWriteIfNeeded();
  const int open_error = errno;
  file_path_.clear();
  FILE* file = file_;
  file_ = NULL;
  const int closed = file ? fclose(file) : 0;
  if (!opened) {
    errno = open_error;
    return false;
  }
  return closed == 0;
}

LoadStatus DepsLog::Load(const string& path, State* state, string* err) {
  METRIC_RECORD(".ninja_deps load");
  char header[kFileSignatureSize];
  FILE* f = FOpenNonInheritable(path.c_str(), "rb");
  if (!f) {
    if (errno == ENOENT)
      return LOAD_NOT_FOUND;
    *err = strerror(errno);
    return LOAD_ERROR;
  }

  DepsRecordReader reader(f);
  bool valid_header = reader.ReadInto(header, kFileSignatureSize) == kFileSignatureSize &&
                      !memcmp(header, kFileSignature, kFileSignatureSize);

  int32_t version = 0;
  bool valid_version =
      reader.ReadInto(&version, 4) == 4 && version == kCurrentVersion;

  // Note: For version differences, this should migrate to the new format.
  // But the v1 format could sometimes (rarely) end up with invalid data, so
  // don't migrate v1 to v3 to force a rebuild. (v2 only existed for a few days,
  // and there was no release with it, so pretend that it never happened.)
  if (!valid_header || !valid_version) {
    if (version == 1)
      *err = "deps log version change; rebuilding";
    else
      *err = "bad deps log signature or version; starting over";
    fclose(f);
    platformAwareUnlink(path.c_str());
    // Don't report this as a failure.  An empty deps log will cause
    // us to rebuild the outputs anyway.
    return LOAD_SUCCESS;
  }

  // long stays 32-bit on Win64. Recovery must retain complete records past
  // 2 GiB instead of sign-extending a wrapped ftell-style offset.
  size_t offset = kFileSignatureSize + sizeof(version);
  bool read_failed = false;
  uint64_t unique_dep_record_count = 0;
  uint64_t total_dep_record_count = 0;
  for (;;) {
    const char* size_data = reader.Read(4);
    if (!size_data) {
      if (!feof(f))
        read_failed = true;
      break;
    }
    unsigned size = ReadWord(size_data);
    bool is_deps = (size >> 31) != 0;
    size = size & 0x7FFFFFFF;

    const char* buf;
    if (!size || size > kMaxRecordSize || !(buf = reader.Read(size))) {
      read_failed = true;
      break;
    }

    if (is_deps) {
      if (size < 12 || (size % 4) != 0) {
        read_failed = true;
        break;
      }
      const uint32_t out_id = ReadWord(buf);
      // A truncated or concurrent write can leave a deps record whose
      // out_id was never assigned a path. Truncate here; storing it
      // made Recompact index nodes_ out of bounds.
      if (out_id >= nodes_.size() || !nodes_[out_id]) {
        read_failed = true;
        break;
      }
      TimeStamp mtime;
      mtime = (TimeStamp)((uint64_t(ReadWord(buf + 8)) << 32) | ReadWord(buf + 4));
      const char* deps_data = buf + 12;
      int deps_count = (size / 4) - 3;

      Deps* deps = Deps::Create(mtime, deps_count, &nodes_);
      for (int i = 0; i < deps_count; ++i) {
        const uint32_t node_id = ReadWord(deps_data + i * 4);
        // Path IDs are assigned densely and every inserted path has a node.
        if (node_id >= nodes_.size()) {
          read_failed = true;
          break;
        }
        deps->nodes.ids[i] = node_id;
      }
      if (read_failed) {
        delete deps;
        break;
      }

      total_dep_record_count++;
      if (!UpdateDeps(out_id, deps))
        ++unique_dep_record_count;
    } else {
      if (size <= 4) {
        read_failed = true;
        break;
      }
      int path_size = size - 4;
      // There can be up to 3 bytes of padding.
      for (int padding = 0; padding < 3 && path_size && buf[path_size - 1] == '\0'; ++padding)
        --path_size;
      if (!path_size) {
        read_failed = true;
        break;
      }
      StringPiece subpath(buf, path_size);
      // It is not necessary to pass in a correct slash_bits here. It will
      // either be a Node that's in the manifest (in which case it will already
      // have a correct slash_bits that GetNode will look up), or it is an
      // implicit dependency from a .d which does not affect the build command
      // (and so need not have its slashes maintained).
      Node* node = state->GetNode(subpath, 0);

      // Check that the expected index matches the actual index. This can only
      // happen if two ninja processes write to the same deps log concurrently.
      // (This uses unary complement to make the checksum look less like a
      // dependency record entry.)
      unsigned checksum = ReadWord(buf + size - 4);
      int expected_id = ~checksum;
      int id = static_cast<int>(nodes_.size());
      if (id != expected_id || node->id() >= 0) {
        read_failed = true;
        break;
      }
      node->set_id(id);
      nodes_.push_back(node);
    }
    offset += size + sizeof(size);
  }

  if (read_failed) {
    // An error occurred while loading; try to recover by truncating the
    // file to the last fully-read record.
    if (ferror(f)) {
      *err = strerror(ferror(f));
    } else {
      *err = "premature end of file";
    }
    fclose(f);

    if (!Truncate(path, offset, err))
      return LOAD_ERROR;

    // The truncate succeeded; we'll just report the load error as a
    // warning because the build can proceed.
    *err += "; recovering";
    return LOAD_SUCCESS;
  }

  fclose(f);

  // Rebuild the log if there are too many dead records.
  constexpr uint64_t kMinCompactionEntryCount = 1000;
  constexpr uint64_t kCompactionRatio = 3;
  if (total_dep_record_count > kMinCompactionEntryCount &&
      total_dep_record_count > unique_dep_record_count * kCompactionRatio) {
    needs_recompaction_ = true;
  }

  return LOAD_SUCCESS;
}

DepsLog::Deps* DepsLog::GetDeps(Node* node) {
  // Abort if the node has no id (never referenced in the deps) or if
  // there's no deps recorded for the node.
  if (node->id() < 0 || node->id() >= (int)deps_.size())
    return NULL;
  return deps_[node->id()];
}

Node* DepsLog::GetFirstReverseDepsNode(Node* node) {
  for (size_t id = 0; id < deps_.size(); ++id) {
    Deps* deps = deps_[id];
    if (!deps)
      continue;
    if (id >= nodes_.size() || !nodes_[id])
      continue;
    for (int i = 0; i < deps->node_count; ++i) {
      if (deps->nodes[i] == node)
        return nodes_[id];
    }
  }
  return NULL;
}

bool DepsLog::Recompact(const string& path, string* err) {
  METRIC_RECORD(".ninja_deps recompact");

  if (!CloseForWrite()) {
    *err = strerror(errno);
    return false;
  }
  string temp_path = path + ".recompact";

  // OpenForWrite() opens for append.  Make sure it's not appending to a
  // left-over file from a previous recompaction attempt that crashed somehow.
  platformAwareUnlink(temp_path.c_str());

  DepsLog new_log;
  const char* flush_mode = getenv("SNINJA_RECOMPACT_FLUSH");
  new_log.flush_on_record_ = flush_mode && strcmp(flush_mode, "1") == 0;
  if (!new_log.OpenForWrite(temp_path, err))
    return false;

  // Clear all known ids so that new ones can be reassigned.  The new indices
  // will refer to the ordering in new_log, not in the current log.
  for (auto i = nodes_.begin(); i != nodes_.end(); ++i)
    (*i)->set_id(-1);
  const auto restore_ids = [this] {
    for (size_t id = 0; id < nodes_.size(); ++id)
      nodes_[id]->set_id(static_cast<int>(id));
  };
  // Borrow live records until commit. The temporary writer only needs new
  // path IDs; allocating and copying a second set of dependency arrays is
  // unnecessary, and would roughly double their peak storage.
  std::vector<Deps*> compacted;

  // Write out all deps again.
  for (int old_id = 0; old_id < (int)deps_.size(); ++old_id) {
    Deps* deps = deps_[old_id];
    if (!deps) continue;  // If nodes_[old_id] is a leaf, it has no deps.
    if (old_id >= (int)nodes_.size() || !nodes_[old_id])
      continue;

    if (!IsDepsEntryLiveFor(nodes_[old_id]))
      continue;

    if (!new_log.RecordDepsImpl<false>(nodes_[old_id], deps->mtime,
                                       deps->node_count, deps->nodes)) {
      *err = strerror(errno);
      new_log.Close();
      restore_ids();
      return false;
    }
    const size_t id = nodes_[old_id]->id();
    if (id >= compacted.size())
      compacted.resize(id + 1);
    compacted[id] = deps;
  }

  // The active log is untouched until every buffered record has reached the
  // OS. In particular, a successful fwrite does not imply a successful close.
  if (!new_log.CloseForWrite()) {
    *err = strerror(errno);
    restore_ids();
    return false;
  }
  // Prepare the remapping before replacing the file. None of the old arrays
  // change until the entire on-disk transaction has succeeded.
  std::vector<uint32_t> remap(nodes_.size());
  for (size_t id = 0; id < nodes_.size(); ++id)
    remap[id] = static_cast<uint32_t>(nodes_[id]->id());
  if (!ReplaceContent(path, temp_path, err)) {
    restore_ids();
    return false;
  }

  for (size_t old_id = 0; old_id < deps_.size(); ++old_id) {
    Deps* deps = deps_[old_id];
    if (!deps)
      continue;
    const int id = old_id < nodes_.size() ? nodes_[old_id]->id() : -1;
    if (id < 0 || static_cast<size_t>(id) >= compacted.size() || compacted[id] != deps)
      delete deps;
  }
  for (Deps* deps : compacted) {
    if (!deps)
      continue;
    for (int i = 0; i < deps->node_count; ++i)
      deps->nodes.ids[i] = remap[deps->nodes.ids[i]];
    deps->nodes.table = &nodes_;
  }
  // All nodes now refer to the new table. Ownership of retained records moves
  // to their new output IDs; the remaining old records were released above.
  deps_.swap(compacted);
  nodes_.swap(new_log.nodes_);

  needs_recompaction_ = false;
  return true;
}

bool DepsLog::IsDepsEntryLiveFor(const Node* node) {
  // Skip entries that don't have in-edges or whose edges don't have a
  // "deps" attribute. They were in the deps log from previous builds, but
  // the the files they were for were removed from the build and their deps
  // entries are no longer needed.
  // (Without the check for "deps", a chain of two or more nodes that each
  // had deps wouldn't be collected in a single recompaction.)
  return node->in_edge() && !node->in_edge()->GetBinding("deps").empty();
}

bool DepsLog::UpdateDeps(int out_id, Deps* deps) {
  if (out_id >= (int)deps_.size())
    deps_.resize(out_id + 1);

  bool delete_old = deps_[out_id] != NULL;
  if (delete_old)
    delete deps_[out_id];
  deps_[out_id] = deps;
  return delete_old;
}

bool DepsLog::RecordId(Node* node) {
  int path_size = static_cast<int>(node->path().size());
  assert(path_size > 0 && "Trying to record empty path Node!");
  int padding = (4 - path_size % 4) % 4;  // Pad path to 4 byte boundary.

  unsigned size = path_size + padding + 4;
  if (size > kMaxRecordSize) {
    errno = ERANGE;
    return false;
  }

  if (!OpenForWriteIfNeeded()) {
    return false;
  }
  int id = static_cast<int>(nodes_.size());
  write_buffer_.resize(1 + size / 4);
  write_buffer_[0] = size;
  char* record = reinterpret_cast<char*>(write_buffer_.data());
  memcpy(record + 4, node->path().data(), path_size);
  memset(record + 4 + path_size, 0, padding);
  write_buffer_.back() = ~static_cast<uint32_t>(id);
  if (fwrite(record, size + 4, 1, file_) < 1)
    return false;
  if (flush_on_record_ && fflush(file_) != 0)
    return false;

  node->set_id(id);
  nodes_.push_back(node);

  return true;
}

bool DepsLog::OpenForWriteIfNeeded() {
  if (file_path_.empty()) {
    return true;
  }
  file_ = FOpenNonInheritable(file_path_.c_str(), "ab");
  if (!file_) {
    return false;
  }
  // Set the buffer size to this and flush the file buffer after every record
  // to make sure records aren't written partially.
  if (setvbuf(file_, NULL, _IOFBF, kMaxRecordSize + 1) != 0) {
    return false;
  }

  // Opening a file in append mode doesn't set the file pointer to the file's
  // end on Windows. Do that explicitly.
  fseek(file_, 0, SEEK_END);

  if (ftell(file_) == 0) {
    if (fwrite(kFileSignature, sizeof(kFileSignature) - 1, 1, file_) < 1) {
      return false;
    }
    if (fwrite(&kCurrentVersion, 4, 1, file_) < 1) {
      return false;
    }
  }
  if (fflush(file_) != 0) {
    return false;
  }
  file_path_.clear();
  return true;
}
