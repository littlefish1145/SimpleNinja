#ifndef SNINJA_MISSING_DEPS_H_
#define SNINJA_MISSING_DEPS_H_

#include <set>
#include <string>
#include <unordered_map>

struct DepsLog;
struct DiskInterface;
struct Edge;
struct Node;
struct Rule;
struct State;

/// Receives one report per missing dependency.
class MissingDependencyScannerDelegate {
 public:
  virtual ~MissingDependencyScannerDelegate() {}
  /// |node| consumes |path|, which is produced by |generator|, but nothing in
  /// the manifest makes |node| wait for |generator|.
  virtual void OnMissingDep(Node* node, const std::string& path,
                            const Rule& generator) = 0;
  virtual void OnStats(int nodes_processed, int nodes_missing_deps,
                       int missing_dep_path_count, int generated_nodes,
                       int generator_rules) = 0;
};

class MissingDependencyPrinter : public MissingDependencyScannerDelegate {
 public:
  void OnMissingDep(Node* node, const std::string& path,
                    const Rule& generator) override;
  void OnStats(int nodes_processed, int nodes_missing_deps,
               int missing_dep_path_count, int generated_nodes,
               int generator_rules) override;
};

/// Looks for targets that depend on a generated file without declaring a
/// dependency on the edge that generates it.
///
/// Such a target builds correctly only by luck -- when something else happens to
/// build the generator first.  In a clean tree, or when that target is built on
/// its own, it fails.  That is what makes `ninja -t missing-deps` worth running
/// after adding a code generator to a build.
struct MissingDependencyScanner {
 public:
  MissingDependencyScanner(MissingDependencyScannerDelegate* delegate,
                           DepsLog* deps_log, State* state,
                           DiskInterface* disk_interface);

  /// Walk the graph from |node|, reporting as it goes.
  void ProcessNode(Node* node);
  void PrintStats();
  bool HadMissingDeps() const { return !nodes_missing_deps_.empty(); }

  void ProcessNodeDeps(Node* node, Node** dep_nodes, int dep_nodes_count);

  /// Is |to| reachable from |from| through explicit (non-depfile) inputs?
  bool PathExistsBetween(Edge* from, Edge* to);

  MissingDependencyScannerDelegate* delegate_;
  DepsLog* deps_log_;
  State* state_;
  DiskInterface* disk_interface_;
  std::set<Node*> seen_;
  std::set<Node*> nodes_missing_deps_;
  std::set<Node*> generated_nodes_;
  std::set<const Rule*> generator_rules_;
  int missing_dep_path_count_;

 private:
  using InnerAdjacencyMap = std::unordered_map<Edge*, bool>;
  using AdjacencyMap = std::unordered_map<Edge*, InnerAdjacencyMap>;
  AdjacencyMap adjacency_map_;
};

#endif  // SNINJA_MISSING_DEPS_H_
