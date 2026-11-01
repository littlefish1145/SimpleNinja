#include "clean.h"

#include <assert.h>
#include <stdio.h>

#include "disk_interface.h"
#include "graph.h"
#include "state.h"
#include "util.h"

using namespace std;

Cleaner::Cleaner(State* state, const BuildConfig& config,
                 DiskInterface* disk_interface)
    : state_(state),
      config_(config),
      disk_interface_(disk_interface),
      cleaned_files_count_(0),
      status_(0) {}

bool Cleaner::FileExists(const string& path) {
  string err;
  TimeStamp mtime = disk_interface_->Stat(path, &err);
  if (mtime == -1)
    Error("%s", err.c_str());
  return mtime > 0;  // treat a Stat() error as "not there"
}

void Cleaner::Report(const string& path) {
  ++cleaned_files_count_;
  if (IsVerbose())
    printf("Remove %s\n", path.c_str());
}

bool Cleaner::IsAlreadyRemoved(const string& path) {
  return removed_.find(path) != removed_.end();
}

void Cleaner::Remove(const string& path) {
  // Several edges can name the same output, and one output can be produced by
  // several rules, so removing twice has to be harmless.
  if (IsAlreadyRemoved(path))
    return;
  removed_.insert(path);
  if (config_.dry_run) {
    if (FileExists(path))
      Report(path);
    return;
  }
  const int ret = disk_interface_->RemoveFile(path);
  if (ret == 0)
    Report(path);
  else if (ret == -1)
    status_ = 1;
}

void Cleaner::RemoveEdgeFiles(Edge* edge) {
  // These are written next to the outputs and are not part of the graph, so
  // they have to be named explicitly or they leak on every rebuild.
  string depfile = edge->GetUnescapedDepfile();
  if (!depfile.empty())
    Remove(depfile);
  string rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty())
    Remove(rspfile);
}

void Cleaner::PrintHeader() {
  if (config_.verbosity == BuildConfig::QUIET)
    return;
  printf("Cleaning...");
  if (IsVerbose())
    printf("\n");
  else
    printf(" ");
  fflush(stdout);
}

void Cleaner::PrintFooter() {
  if (config_.verbosity == BuildConfig::QUIET)
    return;
  printf("%d files.\n", cleaned_files_count_);
}

void Cleaner::Reset() {
  status_ = 0;
  cleaned_files_count_ = 0;
  removed_.clear();
  cleaned_.clear();
}

int Cleaner::CleanAll(bool generator) {
  Reset();
  PrintHeader();
  for (vector<Edge*>::iterator e = state_->edges_.begin();
       e != state_->edges_.end(); ++e) {
    // A phony target is a name, not a file.
    if ((*e)->is_phony())
      continue;
    // Leave generator outputs alone unless asked: they are usually cheap to
    // recreate but expensive to lose (e.g. a regenerated build.ninja).
    if (!generator && (*e)->GetBindingBool("generator"))
      continue;
    for (vector<Node*>::iterator out = (*e)->outputs_.begin();
         out != (*e)->outputs_.end(); ++out)
      Remove((*out)->path());
    RemoveEdgeFiles(*e);
  }
  PrintFooter();
  return status_;
}

int Cleaner::CleanDead(const BuildLog::Entries* entries) {
  Reset();
  PrintHeader();
  for (BuildLog::Entries::const_iterator i = entries->begin();
       i != entries->end(); ++i) {
    // An entry is stale when the manifest no longer knows the path at all, or
    // knows it only as a source file with no edge that produces or consumes it.
    Node* n = state_->LookupNode(i->first);
    if (!n || (!n->in_edge() && n->out_edges().empty()))
      Remove(std::string(i->first));
  }
  PrintFooter();
  return status_;
}

void Cleaner::DoCleanTarget(Node* target) {
  if (Edge* e = target->in_edge()) {
    if (!e->is_phony()) {
      // Remove every output of the edge, not just the one named: a multi-output
      // edge builds all of them from one command, so they have to go together or
      // the next build sees a half-present output.
      for (Node* output : e->outputs_)
        Remove(output->path());
      RemoveEdgeFiles(e);
    }
    for (vector<Node*>::iterator n = e->inputs_.begin(); n != e->inputs_.end();
         ++n) {
      if (cleaned_.count(*n) == 0)
        DoCleanTarget(*n);
    }
  }
  cleaned_.insert(target);
}

int Cleaner::CleanTarget(Node* target) {
  assert(target);
  Reset();
  PrintHeader();
  DoCleanTarget(target);
  PrintFooter();
  return status_;
}

int Cleaner::CleanTarget(const char* target) {
  assert(target);
  Reset();
  Node* node = state_->LookupNode(target);
  if (node)
    return CleanTarget(node);
  Error("unknown target '%s'", target);
  return 1;
}

int Cleaner::CleanTargets(int target_count, char* targets[]) {
  Reset();
  PrintHeader();
  for (int i = 0; i < target_count; ++i) {
    string target_name = targets[i];
    if (target_name.empty()) {
      Error("failed to canonicalize '': empty path");
      status_ = 1;
      continue;
    }
    uint64_t slash_bits;
    CanonicalizePath(&target_name, &slash_bits);
    Node* target = state_->LookupNode(target_name);
    if (!target) {
      Error("unknown target '%s'", target_name.c_str());
      status_ = 1;
      continue;
    }
    if (IsVerbose())
      printf("Target %s\n", target_name.c_str());
    DoCleanTarget(target);
  }
  PrintFooter();
  return status_;
}

void Cleaner::DoCleanRule(const Rule* rule) {
  assert(rule);
  for (vector<Edge*>::iterator e = state_->edges_.begin();
       e != state_->edges_.end(); ++e) {
    if ((*e)->rule().name() != rule->name())
      continue;
    for (vector<Node*>::iterator out = (*e)->outputs_.begin();
         out != (*e)->outputs_.end(); ++out)
      Remove((*out)->path());
    RemoveEdgeFiles(*e);
  }
}

int Cleaner::CleanRule(const Rule* rule) {
  assert(rule);
  Reset();
  PrintHeader();
  DoCleanRule(rule);
  PrintFooter();
  return status_;
}

int Cleaner::CleanRule(const char* rule) {
  assert(rule);
  Reset();
  const Rule* r = state_->bindings_.LookupRule(rule);
  if (r)
    return CleanRule(r);
  Error("unknown rule '%s'", rule);
  return 1;
}

int Cleaner::CleanRules(int rule_count, char* rules[]) {
  assert(rules);
  Reset();
  PrintHeader();
  for (int i = 0; i < rule_count; ++i) {
    const char* rule_name = rules[i];
    const Rule* rule = state_->bindings_.LookupRule(rule_name);
    if (!rule) {
      Error("unknown rule '%s'", rule_name);
      status_ = 1;
      continue;
    }
    if (IsVerbose())
      printf("Rule %s\n", rule_name);
    DoCleanRule(rule);
  }
  PrintFooter();
  return status_;
}
