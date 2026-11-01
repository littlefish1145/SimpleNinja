#ifndef SNINJA_CLEAN_H_
#define SNINJA_CLEAN_H_

#include <set>
#include <string>

#include "build.h"
#include "build_log.h"

struct DiskInterface;
struct Node;
struct Rule;
struct State;

/// Removes the outputs of edges from the manifest (and of the build log).
///
/// Deliberately conservative: it removes what the manifest says a rule produces,
/// plus the response and dep files that belong to the edge.  It does not attempt
/// to follow dyndep files, which sninja does not implement.
struct Cleaner {
  Cleaner(State* state, const BuildConfig& config, DiskInterface* disk_interface);

  /// Clean |target| and every file built to produce it.
  int CleanTarget(Node* target);
  int CleanTarget(const char* target);
  int CleanTargets(int target_count, char* targets[]);

  /// Clean every file in the manifest.  Generator outputs are kept unless
  /// |generator| is set, since regenerating them is usually the point.
  int CleanAll(bool generator = false);

  /// Clean everything built by one rule.
  int CleanRule(const Rule* rule);
  int CleanRule(const char* rule);
  int CleanRules(int rule_count, char* rules[]);

  /// Clean outputs recorded in the build log that the manifest no longer
  /// mentions, i.e. files left behind by a previous configuration.
  int CleanDead(const BuildLog::Entries* entries);

  int cleaned_files_count() const { return cleaned_files_count_; }

  /// Whether to print each file as it is removed.
  bool IsVerbose() const {
    return config_.verbosity != BuildConfig::QUIET &&
           (config_.verbosity == BuildConfig::COMMAND || config_.dry_run);
  }

 private:
  void Remove(const std::string& path);
  bool IsAlreadyRemoved(const std::string& path);
  void RemoveEdgeFiles(Edge* edge);
  void DoCleanTarget(Node* target);
  void DoCleanRule(const Rule* rule);
  void PrintHeader();
  void PrintFooter();
  void Reset();
  bool FileExists(const std::string& path);
  void Report(const std::string& path);

  State* state_;
  const BuildConfig& config_;
  DiskInterface* disk_interface_;
  std::set<std::string> removed_;
  std::set<Node*> cleaned_;
  int cleaned_files_count_;
  int status_;
};

#endif  // SNINJA_CLEAN_H_
