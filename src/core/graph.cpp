#include "graph.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "disk_interface.h"
#include "state.h"
#include "util.h"

using namespace std;

bool Node::Stat(DiskInterface* disk_interface, string* err) {
  mtime_ = disk_interface->Stat(path_, err);
  if (mtime_ == -1)
    return false;
  if (mtime_ == 0)  // File doesn't exist.
    exists_ = ExistenceStatusMissing;
  else
    exists_ = ExistenceStatusExists;
  return true;
}

void Node::UpdatePhonyMtime(TimeStamp mtime) {
  if (!exists()) {
    mtime_ = std::max(mtime_, mtime);
  }
}

bool Edge::AllInputsReady() const {
  for (vector<Node*>::const_iterator i = inputs_.begin(); i != inputs_.end();
       ++i) {
    if ((*i)->in_edge() && !(*i)->in_edge()->outputs_ready())
      return false;
  }
  return true;
}

/// An Env for an Edge, providing $in and $out.
struct EdgeEnv : public Env {
  enum EscapeKind { kShellEscape, kDoNotEscape };

  EdgeEnv(const Edge* const edge, const EscapeKind escape)
      : edge_(edge), escape_in_out_(escape), recursive_(false) {}
  std::string LookupVariable(std::string_view var) override;

  /// Given a span of Nodes, construct a list of paths suitable for a command
  /// line.
  std::string MakePathList(const Node* const* span, size_t size, char sep) const;

 private:
  std::vector<std::string> lookups_;
  const Edge* const edge_;
  EscapeKind escape_in_out_;
  bool recursive_;
};

std::string EdgeEnv::LookupVariable(std::string_view var) {
  if (var == "in" || var == "in_newline") {
    int explicit_deps_count =
        static_cast<int>(edge_->inputs_.size() - edge_->implicit_deps_ -
                         edge_->order_only_deps_);
    return MakePathList(edge_->inputs_.data(), explicit_deps_count,
                        var == "in" ? ' ' : '\n');
  } else if (var == "out") {
    int explicit_outs_count =
        static_cast<int>(edge_->outputs_.size() - edge_->implicit_outs_);
    return MakePathList(&edge_->outputs_[0], explicit_outs_count, ' ');
  }

  // Technical note about the lookups_ vector.
  //
  // This is used to detect cycles during recursive variable expansion
  // which can be seen as a graph traversal problem. The lookups_ vector is
  // used as a stack of visited nodes/variables during recursive expansion.
  // Entering a node adds an item to the stack, leaving the node removes it.
  //
  // The recursive_ flag is used as a small performance optimization
  // to never record the starting node in the stack when beginning a new
  // expansion, since in most cases, expansions are not recursive at all.
  if (recursive_) {
    auto it = std::find_if(lookups_.begin(), lookups_.end(),
                           [var](const std::string& lookup) {
                             return var == lookup;
                           });
    if (it != lookups_.end()) {
      std::string cycle;
      for (; it != lookups_.end(); ++it)
        cycle.append(*it + " -> ");
      cycle.append(var);
      Fatal(("cycle in rule variables: " + cycle).c_str());
    }
  }

  // See notes on BindingEnv::LookupWithFallback.
  const EvalString* eval = edge_->rule_->GetBinding(var);
  bool record_varname = recursive_ && eval;
  if (record_varname)
    lookups_.push_back(std::string(var));

  // In practice, variables defined on rules never use another rule variable.
  // For performance, only start checking for cycles after the first lookup.
  recursive_ = true;
  std::string result = edge_->env_->LookupWithFallback(var, eval, this);
  if (record_varname)
    lookups_.pop_back();
  return result;
}

std::string EdgeEnv::MakePathList(const Node* const* const span,
                                  const size_t size, const char sep) const {
  string result;
  for (const Node* const* i = span; i != span + size; ++i) {
    if (!result.empty())
      result.push_back(sep);
    const string& path = (*i)->PathDecanonicalized();
    if (escape_in_out_ == kShellEscape) {
#ifdef _WIN32
      GetWin32EscapedString(path, &result);
#else
      GetShellEscapedString(path, &result);
#endif
    } else {
      result.append(path);
    }
  }
  return result;
}

std::string Edge::EvaluateCommand(const bool incl_rsp_file) const {
  std::string command = GetBinding("command");
  if (incl_rsp_file) {
    const std::string rspfile_content = GetBinding("rspfile_content");
    if (!rspfile_content.empty())
      command += ";rspfile=" + rspfile_content;
  }
  return command;
}

std::string Edge::GetBinding(std::string_view key) const {
  EdgeEnv env(this, EdgeEnv::kShellEscape);
  return env.LookupVariable(key);
}

bool Edge::GetBindingBool(std::string_view key) const {
  return !GetBinding(key).empty();
}

string Edge::GetUnescapedDepfile() const {
  EdgeEnv env(this, EdgeEnv::kDoNotEscape);
  return env.LookupVariable("depfile");
}

string Edge::GetUnescapedDyndep() const {
  EdgeEnv env(this, EdgeEnv::kDoNotEscape);
  return env.LookupVariable("dyndep");
}

std::string Edge::GetUnescapedRspfile() const {
  EdgeEnv env(this, EdgeEnv::kDoNotEscape);
  return env.LookupVariable("rspfile");
}

void Edge::Dump(const char* prefix) const {
  printf("%s[ ", prefix);
  for (vector<Node*>::const_iterator i = inputs_.begin();
       i != inputs_.end() && *i != NULL; ++i) {
    printf("%s ", (*i)->path().c_str());
  }
  printf("--%s-> ", rule_->name().c_str());
  for (vector<Node*>::const_iterator i = outputs_.begin();
       i != outputs_.end() && *i != NULL; ++i) {
    printf("%s ", (*i)->path().c_str());
  }
  if (!validations_.empty()) {
    printf(" validations ");
    for (std::vector<Node*>::const_iterator i = validations_.begin();
         i != validations_.end() && *i != NULL; ++i) {
      printf("%s ", (*i)->path().c_str());
    }
  }
  if (pool_) {
    if (!pool_->name().empty()) {
      printf("(in pool '%s')", pool_->name().c_str());
    }
  } else {
    printf("(null pool?)");
  }
  printf("] 0x%p\n", this);
}

bool Edge::is_phony() const {
  return rule_->IsPhony();
}

bool Edge::use_console() const {
  return pool() == &State::kConsolePool;
}

bool Edge::maybe_phonycycle_diagnostic() const {
  // CMake 2.8.12.x and 3.0.x produced self-referencing phony rules
  // of the form "build a: phony ... a ...".   Restrict our
  // "phonycycle" diagnostic option to the form it used.
  return is_phony() && outputs_.size() == 1 && implicit_outs_ == 0 &&
         implicit_deps_ == 0 && order_only_deps_ == 0;
}

// static
string Node::PathDecanonicalized(const string& path, uint64_t slash_bits) {
  string result = path;
#ifdef _WIN32
  uint64_t mask = 1;
  for (char* c = &result[0]; (c = strchr(c, '/')) != NULL;) {
    if (slash_bits & mask)
      *c = '\\';
    c++;
    mask <<= 1;
  }
#endif
  return result;
}

void Node::Dump(const char* prefix) const {
  printf("%s <%s 0x%p> mtime: %lld%s, (:%s), ", prefix, path().c_str(), this,
         (long long)mtime(), exists() ? "" : " (:missing)",
         dirty() ? " dirty" : " clean");
  if (in_edge()) {
    in_edge()->Dump("in-edge: ");
  } else {
    printf("no in-edge\n");
  }
  printf(" out edges:\n");
  for (vector<Edge*>::const_iterator e = out_edges().begin();
       e != out_edges().end() && *e != NULL; ++e) {
    (*e)->Dump(" +- ");
  }
}

void InputsCollector::VisitNode(const Node* node) {
  const Edge* edge = node->in_edge();

  if (!edge)  // A source file.
    return;

  // Add inputs of the producing edge to the result,
  // except if they are themselves produced by a phony edge.
  for (const Node* input : edge->inputs_) {
    if (!visited_nodes_.insert(input).second)
      continue;

    VisitNode(input);

    const Edge* input_edge = input->in_edge();
    if (!(input_edge && input_edge->is_phony()))
      inputs_.push_back(input);
  }
}

std::vector<std::string> InputsCollector::GetInputsAsStrings(
    bool shell_escape) const {
  std::vector<std::string> result;
  result.reserve(inputs_.size());

  for (const Node* input : inputs_) {
    std::string unescaped = input->PathDecanonicalized();
    if (shell_escape) {
#ifdef _WIN32
      std::string escaped;
      GetWin32EscapedString(unescaped, &escaped);
      result.push_back(escaped);
#else
      std::string escaped;
      GetShellEscapedString(unescaped, &escaped);
      result.push_back(escaped);
#endif
    } else {
      result.push_back(unescaped);
    }
  }
  return result;
}
