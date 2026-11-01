// Dependency scanning: walks the graph and decides which edges need to run.
//
// This is the heart of incremental builds.  The rules (all matching upstream
// ninja):
//   * a node with no in-edge is dirty iff the file is missing;
//   * an edge is dirty if any non-order-only input is dirty/missing;
//   * an edge is dirty if an output is missing, older than the most recent
//     input, its command hash changed since the recorded log entry, or the
//     recorded mtime in the log is older than the most recent input;
//   * "restat" rules compare only the mtime recorded in the build log, so a
//     command that rewrites identical output does not cascade;
//   * "generator" rules ignore command-hash changes.
#include "graph.h"

#include <inttypes.h>

#include <algorithm>
#include <cassert>
#include <deque>

#include "build_log.h"
#include "disk_interface.h"
#include "util.h"

using namespace std;

namespace {

/// Compute the (single) command hash of an edge lazily: for most builds only a
/// few edges need it.
struct LazyEdgeCommandHash {
  explicit LazyEdgeCommandHash(const Edge* edge) : edge_(edge) {}
  uint64_t operator()() {
    if (!valid_) {
      valid_ = true;
      return command_ =
                 BuildLog::LogEntry::HashCommand(edge_->EvaluateCommand(true));
    }
    return command_;
  }

 private:
  const Edge* edge_;
  uint64_t command_ = 0;
  bool valid_ = false;
};

}  // namespace

bool DependencyScan::RecomputeDirty(Node* initial_node,
                                    std::vector<Node*>* validation_nodes,
                                    string* err) {
  std::vector<Node*> stack;
  std::vector<Node*> new_validation_nodes;

  std::deque<Node*> nodes(1, initial_node);

  // RecomputeNodeDirty might return new validation nodes that need to be
  // checked for dirty state; keep a queue of nodes to visit.
  while (!nodes.empty()) {
    Node* node = nodes.front();
    nodes.pop_front();

    stack.clear();
    new_validation_nodes.clear();

    if (!RecomputeNodeDirty(node, &stack, &new_validation_nodes, err))
      return false;
    nodes.insert(nodes.end(), new_validation_nodes.begin(),
                 new_validation_nodes.end());
    if (!new_validation_nodes.empty()) {
      assert(validation_nodes &&
             "validations require RecomputeDirty to be called with "
             "validation_nodes");
      validation_nodes->insert(validation_nodes->end(),
                               new_validation_nodes.begin(),
                               new_validation_nodes.end());
    }
  }

  return true;
}

/// Recomputes the dirtiness state of the specified input range of the node's
/// incoming edge.
bool DependencyScan::RecomputeEdgesInputsDirty(
    const Node* /*node*/, EdgeInputsRange input_range,
                                 Node*& most_recent_input,
    bool& dirty, std::vector<Node*>* stack,
    std::vector<Node*>* validation_nodes, std::string* err) {
  const auto& edge = input_range.edge_;

  // Visit all specified inputs before checking if any of them is ready.
  for (auto i : input_range) {
    if (!RecomputeNodeDirty(i, stack, validation_nodes, err))
      return false;
  }

  for (auto i = input_range.begin(); i != input_range.end(); ++i) {
    // If an input is not ready, neither are our outputs.
    if (Edge* in_edge = (*i)->in_edge()) {
      if (!in_edge->outputs_ready_)
        edge->outputs_ready_ = false;
    }

    if (!edge->is_order_only(i - edge->inputs_.cbegin())) {
      // If a regular input is dirty (or missing), we're dirty.
      // Otherwise consider mtime.
      if ((*i)->dirty()) {
        dirty = true;
      } else {
        if (!most_recent_input ||
            (*i)->mtime() > most_recent_input->mtime()) {
          most_recent_input = *i;
        }
      }
    }
  }

  return true;
}

bool DependencyScan::RecomputeNodeDirty(Node* node, std::vector<Node*>* stack,
                                        std::vector<Node*>* validation_nodes,
                                        string* err) {
  Edge* edge = node->in_edge();

  if (!edge) {
    // If we already visited this leaf node then we are done.
    if (node->status_known())
      return true;
    // This node has no in-edge; it is dirty if it is missing.
    if (!node->StatIfNecessary(disk_interface_, err))
      return false;
    node->set_dirty(!node->exists());
    return true;
  }

  // If we already finished this edge then we are done.
  if (edge->mark_ == Edge::VisitDone)
    return true;

  // If we encountered this edge earlier in the call stack we have a cycle.
  if (!VerifyDAG(node, stack, err))
    return false;

  // Store any validation nodes from the edge for adding to the initial nodes.
  // Don't recurse into them, that would trigger the dependency cycle detector
  // if the validation node depends on this node.
  validation_nodes->insert(validation_nodes->end(), edge->validations_.begin(),
                           edge->validations_.end());

  // Mark the edge temporarily while in the call stack.
  edge->mark_ = Edge::VisitInStack;
  stack->push_back(node);

  bool dirty = false;
  edge->outputs_ready_ = true;
  edge->deps_missing_ = false;

  const bool edge_deps_loaded = edge->deps_loaded_;
  if (!edge->deps_loaded_) {
    // This is our first encounter with this edge.
    edge->deps_loaded_ = true;
  }

  // Load output mtimes so we can compare them to the most recent input below.
  for (Node* o : edge->outputs_) {
    if (err) {
      *err = "";
    }
    if (!o->StatIfNecessary(disk_interface_, err)) {
      return false;
    }
  }

  Node* most_recent_input = nullptr;
  if (!RecomputeEdgesInputsDirty(node, EdgeInputsRange(node->in_edge()),
                                 most_recent_input, dirty, stack,
                                 validation_nodes, err))
    return false;

  // We may also be dirty due to output state: missing outputs, out of date
  // outputs, etc.
  if (!dirty && !RecomputeOutputsDirtyAll(edge, most_recent_input,
                                          /*allow_restat_mtime_recheck=*/true,
                                          &dirty))
    return false;

  if (!edge_deps_loaded) {
    // Only try to load the deps log if no rebuild is necessary: if a rebuild
    // is necessary the recorded deps are outdated for this target anyway.
    if (!dirty) {
      // Load discovered deps.
      EdgeInputsRange new_deps = EdgeInputsRange::Empty(edge);
      if (!deps_loader_->LoadDeps(edge, &new_deps, err)) {
        // Failed to load dependency info: rebuild to regenerate it.
        dirty = edge->deps_missing_ = true;
      } else if (!new_deps.empty()) {
        // Depfile load succeeded; check the recently added inputs.
        const Node* most_recent_input_previous = most_recent_input;
        if (!RecomputeEdgesInputsDirty(node, new_deps, most_recent_input, dirty,
                                       stack, validation_nodes, err))
          return false;

        // Only applicable if most_recent_input changed; any other criterion has
        // already been checked.
        if (!dirty && most_recent_input_previous != most_recent_input) {
          bool depfile_dirty = false;
          if (!RecomputeOutputsDirtyAll(edge, most_recent_input,
                                        /*allow_restat_mtime_recheck=*/false,
                                        &depfile_dirty))
            return false;
          dirty = depfile_dirty;
        }
      }
    } else if (!deps_loader_->LoadDepsTry(edge, err)) {
      dirty = edge->deps_missing_ = true;
    }
  }

  // Finally, visit each output and update their dirty state if necessary.
  if (dirty) {
    for (auto o : edge->outputs_)
      o->MarkDirty();
  }

  // If an edge is dirty, its outputs are normally not ready.  (It's possible
  // to be clean but still not be ready in the presence of order-only inputs.)
  // But phony edges with no inputs have nothing to do, so are always ready.
  if (dirty && !(edge->is_phony() && edge->inputs_.empty()))
    edge->outputs_ready_ = false;

  // Mark the edge as finished during this walk.
  edge->mark_ = Edge::VisitDone;
  assert(stack->back() == node);
  stack->pop_back();

  return true;
}

bool DependencyScan::VerifyDAG(Node* node, vector<Node*>* stack, string* err) {
  Edge* edge = node->in_edge();
  assert(edge != NULL);

  // If we have no temporary mark on the edge then we do not yet have a cycle.
  if (edge->mark_ != Edge::VisitInStack)
    return true;

  // We have this edge earlier in the call stack.  Find it.
  vector<Node*>::iterator start = stack->begin();
  while (start != stack->end() && (*start)->in_edge() != edge)
    ++start;
  assert(start != stack->end());

  // Make the cycle clear by reporting its start as the node at its end instead
  // of some other output of the starting edge.
  *start = node;

  // Construct the error message rejecting the cycle.
  *err = "dependency cycle: ";
  for (vector<Node*>::const_iterator i = start; i != stack->end(); ++i) {
    err->append((*i)->path());
    err->append(" -> ");
  }
  err->append((*start)->path());

  if ((start + 1) == stack->end() && edge->maybe_phonycycle_diagnostic()) {
    // The manifest parser would have filtered out the self-referencing input if
    // it were not configured to allow the error.
    err->append(" [-w phonycycle=err]");
  }

  return false;
}

/// Recompute whether any output of the edge is dirty, if so sets |*dirty|.
bool DependencyScan::RecomputeOutputsDirty(Edge* edge, Node* most_recent_input,
                                           bool* dirty, string* /*err*/) {
  *dirty = false;
  return RecomputeOutputsDirtyAll(edge, most_recent_input,
                                  /*allow_restat_mtime_recheck=*/true, dirty);
}

bool DependencyScan::RecomputeOutputsDirtyAll(Edge* edge,
                                              Node* most_recent_input,
                                              bool allow_restat_mtime_recheck,
                                              bool* dirty) {
  const bool is_restat = edge->GetBindingBool("restat");
  LazyEdgeCommandHash command_hash(edge);
  bool generator = edge->GetBindingBool("generator");

  for (Node* output : edge->outputs_) {
    const BuildLog::LogEntry* entry =
        build_log_ ? build_log_->LookupByOutput(output->path()) : nullptr;

    if (edge->is_phony()) {
      // Phony edges don't write any output.  Outputs are only dirty if there
      // are no inputs or validations and we're missing the output.
      if (edge->inputs_.empty() && edge->validations_.empty() &&
          !output->exists()) {
        *dirty = true;
        return true;
      }
      // Update the mtime with the newest input, so dependents see a sensible
      // timestamp for the fake node.
      if (most_recent_input)
        output->UpdatePhonyMtime(most_recent_input->mtime());
      continue;
    }

    // Dirty if we're missing the output.
    if (!output->exists()) {
      *dirty = true;
      return true;
    }

    // Dirty if an input is newer than the edge's last successful run.
    //
    // The timestamp an input has to beat depends on the rule:
    //
    //  * For an ordinary rule, the output's own mtime.  Comparing against the
    //    build log's recorded time instead would be wrong in both directions:
    //    a fresh build records the time the command finished, which is *earlier*
    //    than nothing, so an input newer than it would make every edge look
    //    permanently dirty.
    //
    //  * For a restat rule, the mtime recorded in the build log.  The command is
    //    explicitly allowed to leave the output untouched, so the output's real
    //    mtime carries no information -- the recorded time is what the previous
    //    run refreshed from that output, and it is the only thing that says
    //    whether anything has changed since.  This is also what `-t restat`
    //    repairs: after an interrupted run the recorded value can be older than
    //    the inputs, which would make the edge rebuild on every invocation.
    const bool restat_with_entry = is_restat && entry != nullptr;
    const TimeStamp reference_mtime =
        restat_with_entry ? entry->mtime : output->mtime();
    if (most_recent_input &&
        most_recent_input->mtime() > reference_mtime) {
      *dirty = true;
      return true;
    }

    if (entry) {
      // May also be dirty due to the command changing since the last build.
      // But if this is a generator rule, the command changing does not make us
      // dirty.
      if (!generator && command_hash() != entry->command_hash) {
        *dirty = true;
        return true;
      }
    } else if (!generator) {
      // Dirty if the build log has no entry for this output: we have no
      // evidence that the current command produced it.
      *dirty = true;
      return true;
    }
  }


  (void)allow_restat_mtime_recheck;
  *dirty = false;
  return true;
}
