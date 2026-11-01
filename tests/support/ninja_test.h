#ifndef SNINJA_NINJA_TEST_H_
#define SNINJA_NINJA_TEST_H_

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "disk_interface.h"
#include "manifest_parser.h"
#include "state.h"

struct Node;

/// A base test fixture that includes a State object with a
/// builtin "cat" rule.
struct StateTestWithBuiltinRules : public testing::Test {
  StateTestWithBuiltinRules();

  /// Add a "cat" rule to \a state.  Used by some tests; it's
  /// otherwise done by the ctor to state_.
  void AddCatRule(State* state);

  /// Short way to get a Node by its path from state_.
  Node* GetNode(const std::string& path);

  State state_;
};

void AssertParse(State* state, const char* input,
                 ManifestParserOptions = ManifestParserOptions());
void VerifyGraph(const State& state);

/// An implementation of DiskInterface that uses an in-memory representation
/// of disk state.  It also logs file accesses and directory creations
/// so it can be used by tests to verify disk access patterns.
struct VirtualFileSystem : public DiskInterface {
  VirtualFileSystem() : now_(1) {}

  /// "Create" a file with contents.
  void Create(const std::string& path, const std::string& contents);

  /// Tick "time" forwards; subsequent file operations will be newer than
  /// previous ones.
  int Tick() {
    return ++now_;
  }

  // DiskInterface
  TimeStamp Stat(const std::string& path, std::string* err) const override;
  bool WriteFile(const std::string& path, const std::string& contents,
                 bool /*crlf_on_windows*/) override;
  bool MakeDir(const std::string& path) override;
  Status ReadFile(const std::string& path, std::string* contents,
                  std::string* err) override;
  int RemoveFile(const std::string& path) override;

  /// An entry for a single in-memory file.
  struct Entry {
    int mtime;
    std::string stat_error;  // If mtime is -1.
    std::string contents;
  };

  std::vector<std::string> directories_made_;
  std::vector<std::string> files_read_;
  typedef std::map<std::string, Entry> FileMap;
  FileMap files_;
  std::set<std::string> files_removed_;
  std::set<std::string> files_created_;

  /// A simple fake timestamp for file operations.
  int now_;
};

#endif  // SNINJA_NINJA_TEST_H_
