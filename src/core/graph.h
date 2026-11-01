#ifndef SNINJA_GRAPH_H_
#define SNINJA_GRAPH_H_

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "eval_env.h"
#include "timestamp.h"

struct BuildLog;
struct DepsLog;
struct DepfileParserOptions;
struct DiskInterface;
struct DepsLoader;
struct Edge;
struct EdgeInputsRange;
struct Node;
struct Pool;
struct State;

/// Information about a node in the dependency graph: the file, whether
/// it's dirty, mtime, etc.
struct Node {
  Node(const std::string& path, uint64_t slash_bits)
      : path_(path), slash_bits_(slash_bits) {}

  /// Return false on error.
  bool Stat(DiskInterface* disk_interface, std::string* err);

  /// If the file doesn't exist, set the mtime_ from its dependencies.
  void UpdatePhonyMtime(TimeStamp mtime);

  /// Return false on error.
  bool StatIfNecessary(DiskInterface* disk_interface, std::string* err) {
    if (status_known())
      return true;
    return Stat(disk_interface, err);
  }

  /// Mark as not-yet-stat()ed and not dirty.
  void ResetState() {
    mtime_ = -1;
    exists_ = ExistenceStatusUnknown;
    dirty_ = false;
  }

  /// Mark the Node as already-stat()ed and missing.
  void MarkMissing() {
    if (mtime_ == -1) {
      mtime_ = 0;
    }
    exists_ = ExistenceStatusMissing;
  }

  bool exists() const { return exists_ == ExistenceStatusExists; }
  bool status_known() const { return exists_ != ExistenceStatusUnknown; }

  const std::string& path() const { return path_; }
  std::string_view path_view() const { return path_; }
  /// Get |path()| but use slash_bits to convert back to original slash styles.
  std::string PathDecanonicalized() const {
    return PathDecanonicalized(path_, slash_bits_);
  }
  static std::string PathDecanonicalized(const std::string& path,
                                         uint64_t slash_bits);
  uint64_t slash_bits() const { return slash_bits_; }

  TimeStamp mtime() const { return mtime_; }

  bool dirty() const { return dirty_; }
  void set_dirty(bool dirty) { dirty_ = dirty; }
  void MarkDirty() { dirty_ = true; }

  /// Store whether dyndep information is expected from this node but has not
  /// yet been loaded.
  bool dyndep_pending() const { return dyndep_pending_; }
  void set_dyndep_pending(bool pending) { dyndep_pending_ = pending; }

  Edge* in_edge() const { return in_edge_; }
  void set_in_edge(Edge* edge) { in_edge_ = edge; }

  bool generated_by_dep_loader() const { return generated_by_dep_loader_; }
  void set_generated_by_dep_loader(bool value) {
    generated_by_dep_loader_ = value;
  }

  int id() const { return id_; }
  void set_id(int id) { id_ = id; }

  const std::vector<Edge*>& out_edges() const { return out_edges_; }
  const std::vector<Edge*>& validation_out_edges() const {
    return validation_out_edges_;
  }
  void AddOutEdge(Edge* edge) { out_edges_.push_back(edge); }
  void RemoveOutEdge(Edge* edge) {
    out_edges_.erase(std::remove(out_edges_.begin(), out_edges_.end(), edge),
                     out_edges_.end());
  }
  void AddValidationOutEdge(Edge* edge) { validation_out_edges_.push_back(edge); }

  void Dump(const char* prefix = "") const;

 private:
  std::string path_;

  /// Set bits starting from lowest for backslashes that were normalized to
  /// forward slashes by CanonicalizePath. See |PathDecanonicalized|.
  uint64_t slash_bits_ = 0;

  /// Possible values of mtime_:
  ///   -1: file hasn't been examined
  ///   0:  we looked, and file doesn't exist
  ///   >0: actual file's mtime, or the latest mtime of its dependencies if it
  ///       doesn't exist
  TimeStamp mtime_ = -1;

  enum ExistenceStatus : char {
    /// The file hasn't been examined.
    ExistenceStatusUnknown,
    /// The file doesn't exist. mtime_ will be the latest mtime of its
    /// dependencies.
    ExistenceStatusMissing,
    /// The path is an actual file. mtime_ will be the file's mtime.
    ExistenceStatusExists
  };
  ExistenceStatus exists_ = ExistenceStatusUnknown;

  /// Dirty is true when the underlying file is out-of-date.
  bool dirty_ = false;

  /// Store whether dyndep information is expected from this node but has not
  /// yet been loaded.
  bool dyndep_pending_ = false;

  /// Set to true when this node comes from a depfile, a dyndep file or the
  /// deps log. If it does not have a producing edge, the build should not
  /// abort if it is missing (as for regular source inputs).
  bool generated_by_dep_loader_ = true;


  /// A dense integer id for the node, assigned and used by DepsLog.
  int id_ = -1;

  /// The Edge that produces this Node, or NULL when there is no
  /// known edge to produce it.
  Edge* in_edge_ = nullptr;

  /// All Edges that use this Node as an input.
  std::vector<Edge*> out_edges_;

  /// All Edges that use this Node as a validation.
  std::vector<Edge*> validation_out_edges_;
};

/// An edge in the dependency graph; links between Nodes using Rules.
struct Edge {
  enum VisitMark : char {
    VisitNone,
    VisitInStack,
    VisitDone
  };

  Edge() = default;

  /// Return true if all inputs' in-edges are ready.
  bool AllInputsReady() const;

  /// Expand all variables in a command and return it as a string.
  /// If incl_rsp_file is enabled, the string will also contain the
  /// full contents of a response file (if applicable)
  /// Evaluate |command|, substituting $in / $out / $in_newline.  The result is
  /// memoised: a single edge's command is evaluated up to three times per build
  /// (once for the command hash during the dirty scan, once to log it, and once
  /// to hand it to CreateProcess), and expanding it means walking every input
  /// and output and shell-escaping each path.
  std::string EvaluateCommand(bool incl_rsp_file = false) const;
  /// Drop the memoised command; needed if the edge's inputs or outputs change
  /// after it has been evaluated.
  void ClearCommandCache() const {
    command_cache_valid_ = false;
    command_cache_.clear();
    rsp_command_cache_valid_ = false;
    rsp_command_cache_.clear();
  }

  /// Returns the shell-escaped value of |key|.
  std::string GetBinding(std::string_view key) const;
  bool GetBindingBool(std::string_view key) const;

  /// Like GetBinding("depfile"), but without shell escaping.
  std::string GetUnescapedDepfile() const;
  /// Like GetBinding("dyndep"), but without shell escaping.
  std::string GetUnescapedDyndep() const;
  /// Like GetBinding("rspfile"), but without shell escaping.
  std::string GetUnescapedRspfile() const;

  void Dump(const char* prefix = "") const;

  const Rule* rule_ = nullptr;
  Pool* pool_ = nullptr;
  std::vector<Node*> inputs_;
  std::vector<Node*> outputs_;
  std::vector<Node*> validations_;
  Node* dyndep_ = nullptr;
  BindingEnv* env_ = nullptr;
  size_t id_ = 0;

  VisitMark mark_ = VisitNone;
  bool outputs_ready_ = false;
  bool deps_loaded_ = false;
  bool deps_missing_ = false;
  bool generated_by_dep_loader_ = false;
  TimeStamp command_start_time_ = 0;

  // Memoised EvaluateCommand results.  Mutable because evaluation is logically
  // const, and guarded in practice by the fact that the graph is single-threaded
  // until the build starts.
  mutable std::string command_cache_;
  mutable std::string rsp_command_cache_;
  mutable bool command_cache_valid_ = false;
  mutable bool rsp_command_cache_valid_ = false;

  const Rule& rule() const { return *rule_; }
  Pool* pool() const { return pool_; }
  int weight() const { return 1; }
  bool outputs_ready() const { return outputs_ready_; }

  // There are three types of inputs.
  // 1) explicit deps, which show up as $in on the command line;
  // 2) implicit deps, which the target depends on implicitly (e.g. C headers),
  //                   and changes in them cause the target to rebuild;
  // 3) order-only deps, which are needed before the target builds but which
  //                     don't cause the target to rebuild.
  // These are stored in inputs_ in that order, and we keep counts of
  // #2 and #3 when we need to access the various subsets.
  int implicit_deps_ = 0;
  int order_only_deps_ = 0;
  bool is_implicit(size_t index) {
    return index >= inputs_.size() - order_only_deps_ - implicit_deps_ &&
           !is_order_only(index);
  }
  bool is_order_only(size_t index) {
    return index >= inputs_.size() - order_only_deps_;
  }

  // There are two types of outputs.
  // 1) explicit outs, which show up as $out on the command line;
  // 2) implicit outs, which the target generates but are not part of $out.
  int implicit_outs_ = 0;
  bool is_implicit_out(size_t index) const {
    return index >= outputs_.size() - implicit_outs_;
  }

  bool is_phony() const;
  bool use_console() const;
  bool maybe_phonycycle_diagnostic() const;

  // Historical info: how long did this edge take last time,
  // as per .ninja_log, if known? Defaults to -1 if unknown.
  int64_t prev_elapsed_time_millis = -1;
};

struct EdgeCmp {
  bool operator()(const Edge* a, const Edge* b) const {
    return a->id_ < b->id_;
  }
};

typedef std::set<Edge*, EdgeCmp> EdgeSet;

/// A lightweight range of an Edge's input nodes.
///
/// A default-constructed `EdgeInputsRange` spans the entire `edge->inputs_`
/// vector.
struct EdgeInputsRange {
  using const_iterator = std::vector<Node*>::const_iterator;

  /// Create a new instance covering all |edge| inputs.
  EdgeInputsRange(Edge* edge)
      : edge_(edge), beg_(edge->inputs_.begin()), end_(edge->inputs_.end()) {}

  EdgeInputsRange(Edge* edge, const_iterator beg, const_iterator end)
      : edge_(edge), beg_(beg), end_(end) {}

  static EdgeInputsRange Empty(Edge* edge) {
    return EdgeInputsRange(edge, edge->inputs_.begin(), edge->inputs_.begin());
  }

  const_iterator begin() const { return beg_; }
  const_iterator end() const { return end_; }
  size_t size() const { return static_cast<size_t>(end_ - beg_); }
  bool empty() const { return beg_ == end_; }

  /// The edge whose input range is being viewed.
  Edge* edge_;

 private:
  const_iterator beg_;
  const_iterator end_;
};

/// A class used to collect the transitive set of inputs from a given set
/// of starting nodes.  Used to implement the `inputs` tool.
///
/// When collecting inputs, the outputs of phony edges are always ignored
/// from the result, but are followed by the dependency walk.
struct InputsCollector {
  /// Visit a single @arg node during this collection.
  void VisitNode(const Node* node);

  /// Retrieve list of visited input nodes. A dependency always appears
  /// before its dependents in the result, but final order depends on the
  /// order of the VisitNode() calls performed before this.
  const std::vector<const Node*>& inputs() const { return inputs_; }

  /// Same as inputs(), but returns the list of visited nodes as a list of
  /// strings, with optional shell escaping.
  std::vector<std::string> GetInputsAsStrings(bool shell_escape = false) const;

  /// Reset collector state.
  void Reset() {
    inputs_.clear();
    visited_nodes_.clear();
  }

 private:
  std::vector<const Node*> inputs_;
  std::set<const Node*> visited_nodes_;
};

/// Interface for loading extra inputs that are discovered outside the
/// manifest: depfiles and the deps log.
///
/// Milestone 3 replaces the trivial implementation with the real one; keeping
/// it behind an interface lets DependencyScan stay free of deps-log details.
struct DepsLoader {
  virtual ~DepsLoader() = default;

  /// Load any newly discovered inputs for |edge| and append them to the edge.
  /// On success, |*added| is set to the range of inputs that were added
  /// (possibly empty).  Returns false on error, with |err| set.
  virtual bool LoadDeps(Edge* edge, EdgeInputsRange* added, std::string* err) = 0;

  /// A non-mutating check of whether dependency information is available for
  /// |edge|; used when the edge is already known to be dirty.
  virtual bool LoadDepsTry(const Edge* edge, std::string* err) const = 0;
};

/// A DepsLoader that never finds anything: correct for manifests without
/// depfiles or a deps log.
struct NullDepsLoader : DepsLoader {
  bool LoadDeps(Edge* edge, EdgeInputsRange* added,
                std::string* /*err*/) override {
    *added = EdgeInputsRange::Empty(edge);
    return true;
  }
  bool LoadDepsTry(const Edge* /*edge*/, std::string* /*err*/) const override {
    return true;
  }
};

/// DependencyScan manages the process of scanning the files in a graph
/// and updating the dirty/outputs_ready state of all the nodes and edges.
struct DependencyScan {
  DependencyScan(BuildLog* build_log, DiskInterface* disk_interface,
                 DepsLoader* deps_loader)
      : build_log_(build_log), disk_interface_(disk_interface),
        deps_loader_(deps_loader ? deps_loader : &default_deps_loader()) {}

  /// Update the |dirty_| state of the given nodes by transitively inspecting
  /// their input edges.
  /// Examine inputs, outputs, and command lines to judge whether an edge
  /// needs to be re-run, and update outputs_ready_ and each outputs' |dirty_|
  /// state accordingly.
  /// Appends any validation nodes found to the nodes parameter.
  /// Returns false on failure.
  bool RecomputeDirty(Node* node, std::vector<Node*>* validation_nodes,
                      std::string* err);

  /// Recompute whether any output of the edge is dirty, if so sets |*dirty|.
  bool RecomputeOutputsDirty(Edge* edge, Node* most_recent_input, bool* dirty,
                             std::string* err);

  BuildLog* build_log() const { return build_log_; }
  void set_build_log(BuildLog* log) { build_log_ = log; }

  DepsLoader* set_deps_loader(DepsLoader* loader) {
    deps_loader_ = loader;
    return deps_loader_;
  }

 private:
  static DepsLoader& default_deps_loader() {
    static NullDepsLoader instance;
    return instance;
  }

  bool RecomputeNodeDirty(Node* node, std::vector<Node*>* stack,
                          std::vector<Node*>* validation_nodes,
                          std::string* err);
  bool RecomputeEdgesInputsDirty(const Node* node, EdgeInputsRange input_range,
                                 Node*& most_recent_input, bool& dirty,
                                 std::vector<Node*>* stack,
                                 std::vector<Node*>* validation_nodes,
                                 std::string* err);
  bool VerifyDAG(Node* node, std::vector<Node*>* stack, std::string* err);

  /// Whether any output of |edge| is out of date.
  bool RecomputeOutputsDirtyAll(Edge* edge, Node* most_recent_input,
                                bool allow_restat_mtime_recheck,
                                bool* dirty);

  BuildLog* build_log_;
  DiskInterface* disk_interface_;
  DepsLoader* deps_loader_;
};

#endif  // SNINJA_GRAPH_H_
