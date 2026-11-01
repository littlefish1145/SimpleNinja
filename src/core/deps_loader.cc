#include "deps_loader.h"

#include <inttypes.h>

#include <algorithm>

#include "depfile_parser.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "state.h"
#include "util.h"

using namespace std;

bool ImplicitDepLoader::LoadDeps(Edge* edge, EdgeInputsRange* added,
                                 string* err) {
  string deps_type = edge->GetBinding("deps");
  if (!deps_type.empty())
    return LoadDepsFromLog(edge, added, err);

  string depfile = edge->GetUnescapedDepfile();
  if (!depfile.empty())
    return LoadDepFile(edge, depfile, added, err);

  // No deps to load.
  *added = EdgeInputsRange::Empty(edge);
  return true;
}

bool ImplicitDepLoader::LoadDepsTry(const Edge* edge, string* err) const {
  string deps_type = edge->GetBinding("deps");
  if (!deps_type.empty())
    return LoadDepsFromLogTry(edge, err);

  string depfile = edge->GetUnescapedDepfile();
  if (!depfile.empty())
    return LoadDepFileTry(edge, depfile, err);

  // No deps to load.
  return true;
}

bool ImplicitDepLoader::LoadDepsFromLog(Edge* edge, EdgeInputsRange* added,
                                        string* err) {
  // NOTE: deps are only supported for single-target edges.
  Node* output = edge->outputs_[0];
  DepsLog::Deps* deps = deps_log_ ? deps_log_->GetDeps(output) : NULL;
  if (!deps) {
    // Deps are missing: the edge must be rebuilt to regenerate them.
    return false;
  }

  // Load the output's mtime if we haven't already.
  if (!output->StatIfNecessary(disk_interface_, err)) {
    return false;
  }

  // Deps are invalid if the output is newer than the deps.
  if (output->mtime() > deps->mtime) {
    return false;
  }

  Node** nodes = deps->nodes;
  const size_t node_count = deps->node_count;
  const auto implicit_dep = edge->inputs_.end() - edge->order_only_deps_;

  edge->implicit_deps_ += static_cast<int>(node_count);
  for (size_t i = 0; i < node_count; ++i) {
    nodes[i]->AddOutEdge(edge);
  }

  const auto begin = edge->inputs_.insert(implicit_dep, nodes, nodes + node_count);
  *added = EdgeInputsRange(edge, begin, begin + node_count);
  return true;
}

bool ImplicitDepLoader::LoadDepsFromLogTry(const Edge* edge,
                                           string* err) const {
  // NOTE: deps are only supported for single-target edges.
  Node* output = edge->outputs_[0];
  DepsLog::Deps* deps = deps_log_ ? deps_log_->GetDeps(output) : NULL;
  if (!deps) {
    err->clear();
    return false;
  }

  // Load the output's mtime if we haven't already.
  if (!output->StatIfNecessary(disk_interface_, err)) {
    return false;
  }

  // Deps are invalid if the output is newer than the deps.
  if (output->mtime() > deps->mtime) {
    return false;
  }

  return true;
}

bool ImplicitDepLoader::LoadDepFile(Edge* edge, const string& path,
                                    EdgeInputsRange* added, string* err) {
  // Read depfile content.  Treat a missing depfile as empty.
  string content;
  switch (disk_interface_->ReadFile(path, &content, err)) {
    case DiskInterface::Okay:
      break;
    case DiskInterface::NotFound:
      err->clear();
      break;
    case DiskInterface::OtherError:
      *err = "loading '" + path + "': " + *err;
      return false;
  }
  // On a missing depfile: return false and empty *err.
  Node* first_output = edge->outputs_[0];
  if (content.empty()) {
    return false;
  }

  DepfileParser depfile;
  string depfile_err;
  if (!depfile.Parse(&content, &depfile_err)) {
    *err = path + ": " + depfile_err;
    return false;
  }

  if (depfile.outs_.empty()) {
    *err = path + ": no outputs declared";
    return false;
  }

  uint64_t unused;
  std::string_view primary_out = depfile.outs_[0];
  size_t primary_out_len = primary_out.size();
  CanonicalizePath(const_cast<char*>(primary_out.data()), &primary_out_len,
                   &unused);
  primary_out = std::string_view(primary_out.data(), primary_out_len);

  // Check that this depfile matches the edge's output, if not return false to
  // mark the edge as dirty.
  std::string_view opath = first_output->path();
  if (opath != primary_out) {
    return false;
  }

  // Ensure that all mentioned outputs are outputs of the edge.
  for (std::string_view o : depfile.outs_) {
    bool found = false;
    for (Node* out : edge->outputs_) {
      if (out->path() == o) {
        found = true;
        break;
      }
    }
    if (!found) {
      *err = path + ": depfile mentions '" + string(o) +
             "' as an output, but no such output was declared";
      return false;
    }
  }

  return ProcessDepfileDeps(edge, added, &depfile.ins_, err);
}

bool ImplicitDepLoader::LoadDepFileTry(const Edge* /*edge*/,
                                       const string& path,
                                       string* err) const {
  TimeStamp time_stamp = disk_interface_->Stat(path, err);

  if (time_stamp > 0)
    return true;
  else if (time_stamp == 0) {
    *err = "";
    return false;
  } else {
    *err = "loading '" + path + "': " + *err;
    return false;
  }
}

bool ImplicitDepLoader::ProcessDepfileDeps(
    Edge* edge, EdgeInputsRange* added, std::vector<std::string_view>* depfile_ins,
    string* /*err*/) {
  // Preallocate space in edge->inputs_ to be filled in below.
  vector<Node*>::iterator implicit_dep =
      PreallocateSpace(edge, static_cast<int>(depfile_ins->size()));
  const auto beg = implicit_dep;

  // Add all its in-edges.
  for (std::string_view i : *depfile_ins) {
    uint64_t slash_bits;
    size_t len = i.size();
    CanonicalizePath(const_cast<char*>(i.data()), &len, &slash_bits);
    Node* node = state_->GetNode(std::string_view(i.data(), len), slash_bits);
    *implicit_dep = node;
    node->AddOutEdge(edge);
    ++implicit_dep;
  }

  const auto end = implicit_dep;
  *added = EdgeInputsRange(edge, beg, end);
  return true;
}

vector<Node*>::iterator ImplicitDepLoader::PreallocateSpace(Edge* edge,
                                                            int count) {
  edge->inputs_.insert(edge->inputs_.end() - edge->order_only_deps_,
                       (size_t)count, 0);
  edge->implicit_deps_ += count;
  return edge->inputs_.end() - edge->order_only_deps_ - count;
}
