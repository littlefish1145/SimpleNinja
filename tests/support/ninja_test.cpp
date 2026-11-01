#include "ninja_test.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <algorithm>
#include <set>

#include "graph.h"
#include "manifest_parser.h"
#include "util.h"

using namespace std;

StateTestWithBuiltinRules::StateTestWithBuiltinRules() {
  AddCatRule(&state_);
}

void StateTestWithBuiltinRules::AddCatRule(State* state) {
  AssertParse(state,
"rule cat\n"
"  command = cat $in > $out\n");
}

Node* StateTestWithBuiltinRules::GetNode(const string& path) {
  EXPECT_FALSE(strpbrk(path.c_str(), "/\\"));
  return state_.GetNode(path, 0);
}

void AssertParse(State* state, const char* input, ManifestParserOptions opts) {
  ManifestParser parser(state, NULL, opts);
  string err;
  EXPECT_TRUE(parser.ParseTest(input, &err));
  ASSERT_EQ("", err);
  VerifyGraph(*state);
}

void VerifyGraph(const State& state) {
  for (vector<Edge*>::const_iterator e = state.edges_.begin();
       e != state.edges_.end(); ++e) {
    // All edges need at least one output.
    EXPECT_FALSE((*e)->outputs_.empty());
    // Check that the edge's inputs have the edge as out-edge.
    for (vector<Node*>::const_iterator in_node = (*e)->inputs_.begin();
         in_node != (*e)->inputs_.end(); ++in_node) {
      const vector<Edge*>& out_edges = (*in_node)->out_edges();
      EXPECT_NE(find(out_edges.begin(), out_edges.end(), *e), out_edges.end());
    }
    // Check that the edge's outputs have the edge as in-edge.
    for (vector<Node*>::const_iterator out_node = (*e)->outputs_.begin();
         out_node != (*e)->outputs_.end(); ++out_node) {
      EXPECT_EQ((*out_node)->in_edge(), *e);
    }
  }

  // The union of all in- and out-edges of each nodes should be exactly
  // edges_.
  set<const Edge*> node_edge_set;
  for (State::Paths::const_iterator p = state.paths_.begin();
       p != state.paths_.end(); ++p) {
    const Node* n = p->second;
    if (n->in_edge())
      node_edge_set.insert(n->in_edge());
    node_edge_set.insert(n->out_edges().begin(), n->out_edges().end());
  }
  set<const Edge*> edge_set(state.edges_.begin(), state.edges_.end());
  EXPECT_EQ(node_edge_set, edge_set);
}

void VirtualFileSystem::Create(const string& path, const string& contents) {
  files_[path].mtime = now_;
  files_[path].contents = contents;
  files_created_.insert(path);
}

TimeStamp VirtualFileSystem::Stat(const string& path, string* err) const {
  FileMap::const_iterator i = files_.find(path);
  if (i != files_.end()) {
    if (!i->second.stat_error.empty()) {
      *err = i->second.stat_error;
      return -1;
    }
    assert(i->second.mtime > 0);
    return i->second.mtime;
  }
  return 0;
}

bool VirtualFileSystem::WriteFile(const string& path, const string& contents,
                                  bool /*crlf_on_windows*/) {
  Create(path, contents);
  return true;
}

bool VirtualFileSystem::MakeDir(const string& path) {
  directories_made_.push_back(path);
  return true;  // success
}

FileReader::Status VirtualFileSystem::ReadFile(const string& path,
                                               string* contents, string* err) {
  files_read_.push_back(path);
  FileMap::iterator i = files_.find(path);
  if (i != files_.end()) {
    *contents = i->second.contents;
    return Okay;
  }
  *err = strerror(ENOENT);
  return NotFound;
}

int VirtualFileSystem::RemoveFile(const string& path) {
  if (find(directories_made_.begin(), directories_made_.end(), path) !=
      directories_made_.end())
    return -1;
  FileMap::iterator i = files_.find(path);
  if (i != files_.end()) {
    files_.erase(i);
    files_removed_.insert(path);
    return 0;
  } else {
    return 1;
  }
}
