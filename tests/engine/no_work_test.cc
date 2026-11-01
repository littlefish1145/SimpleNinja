#include "build_log.h"
#include "deps_log.h"
#include "depfile_parser.h"
#include "graph.h"
#include "test.h"

namespace {
struct NoWork : testing::Test {
  ScopedFilePath file{"NoWork-deps"};
  State state;
  VirtualFileSystem fs;
  BuildLog build_log;
  DepsLog deps_log;

  void SetUp() override {
    std::string err;
    ASSERT_TRUE(deps_log.OpenForWrite(file.path(), &err));
    AssertParse(&state, "rule cc\n  command = cc $in -o $out\n  deps = gcc\n"
                        "build out: cc src\nbuild all: phony out\n");
    fs.Create("src", "source");
    fs.Create("header", "header");
    fs.Tick();
    fs.Create("out", "object");
    fs.Tick();
    Record("out", {state.GetNode("header", 0)});
  }
  void Record(const char* output, std::vector<Node*> inputs) {
    Node* node = state.LookupNode(output);
    std::string err;
    const TimeStamp mtime = fs.Stat(output, &err);
    ASSERT_TRUE(build_log.RecordCommand(node->in_edge(), 0, 1, mtime));
    ASSERT_TRUE(deps_log.RecordDeps(node, mtime, inputs));
  }
  bool Probe(std::vector<Node*> targets = {}) {
    if (targets.empty()) targets.push_back(state.LookupNode("all"));
    return TryNoWork(&state, &build_log, &deps_log, &fs, targets);
  }
};
}

TEST_F(NoWork, ProvesCleanLoggedSourcesWithoutMaterializingTheGraph) {
  Node* output = state.LookupNode("out");
  Node* header = state.LookupNode("header");
  for (int repeat = 0; repeat < 3; ++repeat) {
    ASSERT_TRUE(Probe());
    EXPECT_EQ(1u, output->in_edge()->inputs_.size());
    EXPECT_TRUE(header->out_edges().empty());
    EXPECT_FALSE(output->status_known());
    EXPECT_FALSE(header->status_known());
    EXPECT_FALSE(state.LookupNode("all")->status_known());
    EXPECT_EQ(Edge::VisitNone, output->in_edge()->mark_);
    EXPECT_FALSE(output->in_edge()->deps_loaded_);
    EXPECT_FALSE(output->in_edge()->outputs_ready_);
  }
}

TEST_F(NoWork, DirtyAndMissingHeadersFallBackWithPristineState) {
  Node* header = state.LookupNode("header");
  fs.Create("header", "new header");
  EXPECT_FALSE(Probe());
  EXPECT_FALSE(header->status_known());
  EXPECT_FALSE(state.LookupNode("out")->status_known());
  ASSERT_EQ(0, fs.RemoveFile("header"));
  EXPECT_FALSE(Probe());
  EXPECT_FALSE(header->status_known());
  DepfileParserOptions options;
  DependencyScan scan(&state, &build_log, &deps_log, &fs, &options, nullptr);
  std::vector<Node*> validations;
  std::string err;
  ASSERT_TRUE(scan.RecomputeDirty(state.LookupNode("out"), &validations, &err));
  EXPECT_TRUE(state.LookupNode("out")->dirty());
  EXPECT_EQ(2u, state.LookupNode("out")->in_edge()->inputs_.size());
  EXPECT_EQ(1u, header->out_edges().size());
}

TEST_F(NoWork, PreservesKnownPhonyMtimesOnSuccessAndLateFallback) {
  Node* all = state.LookupNode("all");
  std::string err;
  ASSERT_TRUE(all->Stat(&fs, &err));
  all->UpdatePhonyMtime(1);
  ASSERT_TRUE(Probe());
  EXPECT_TRUE(all->status_known());
  EXPECT_FALSE(all->exists());
  EXPECT_EQ(1, all->mtime());
  Node* missing = state.GetNode("missing", 0);
  EXPECT_FALSE(Probe({all, missing}));
  EXPECT_EQ(1, all->mtime());
  EXPECT_FALSE(missing->status_known());
  EXPECT_FALSE(state.LookupNode("out")->status_known());
}

TEST_F(NoWork, GeneratedDependenciesAndCyclesUseTheCompleteScanner) {
  AssertParse(&state, "build header: cc out\n");
  fs.Create("header", "generated");
  EXPECT_FALSE(Probe());
  EXPECT_FALSE(state.LookupNode("out")->status_known());
  EXPECT_FALSE(state.LookupNode("header")->status_known());
  DepfileParserOptions options;
  DependencyScan scan(&state, &build_log, &deps_log, &fs, &options, nullptr);
  std::vector<Node*> validations;
  std::string err;
  EXPECT_FALSE(scan.RecomputeDirty(state.LookupNode("out"), &validations, &err));
  EXPECT_NE(std::string::npos, err.find("dependency cycle:"));
}

TEST_F(NoWork, RestatUsesRecordedMtimeAndGeneratorIgnoresCommandChange) {
  Edge* edge = state.LookupNode("out")->in_edge();
  edge->env_ = state.NewBindingEnv(edge->env_);
  edge->env_->AddBinding("restat", "1");
  fs.Create("header", "updated");
  ASSERT_TRUE(build_log.RecordCommand(edge, 0, 1, fs.now_));
  EXPECT_TRUE(Probe());
  edge->env_->AddBinding("command", "different command");
  EXPECT_FALSE(Probe());
  edge->env_->AddBinding("generator", "1");
  EXPECT_TRUE(Probe());
}

TEST_F(NoWork, ValidationDyndepDepfileAndPreviouslyScannedEdgesFallBack) {
  Edge* edge = state.LookupNode("out")->in_edge();
  edge->validations_.push_back(state.GetNode("check", 0));
  EXPECT_FALSE(Probe());
  edge->validations_.clear();
  edge->dyndep_ = state.GetNode("graph.dd", 0);
  EXPECT_FALSE(Probe());
  edge->dyndep_ = nullptr;
  edge->deps_loaded_ = true;
  EXPECT_FALSE(Probe());
  edge->deps_loaded_ = false;
  edge->env_ = state.NewBindingEnv(edge->env_);
  edge->env_->AddBinding("deps", "");
  edge->env_->AddBinding("depfile", "out.d");
  EXPECT_FALSE(Probe());
  EXPECT_FALSE(state.LookupNode("out")->status_known());
}

TEST_F(NoWork, RetainedRawStatsPreserveMissingLeafAndPhonyScanSemantics) {
  ASSERT_EQ(0, fs.RemoveFile("header"));
  ASSERT_FALSE(TryNoWork(&state, &build_log, &deps_log, &fs,
                        {state.LookupNode("all")}, true));
  EXPECT_TRUE(state.LookupNode("header")->status_known());
  EXPECT_TRUE(state.LookupNode("header")->dirty());
  // The speculative phony mtime is not a physical stat result.
  EXPECT_EQ(0, state.LookupNode("all")->mtime());
  DepfileParserOptions options;
  DependencyScan scan(&state, &build_log, &deps_log, &fs, &options, nullptr);
  std::vector<Node*> validations;
  std::string err;
  ASSERT_TRUE(scan.RecomputeDirty(state.LookupNode("all"), &validations, &err));
  EXPECT_TRUE(state.LookupNode("out")->dirty());
  EXPECT_TRUE(state.LookupNode("all")->dirty());
}
