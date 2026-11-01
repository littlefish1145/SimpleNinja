#include "deps_log.h"
#include "graph.h"
#include "state.h"
#include "test.h"

#include <cerrno>
#include <fstream>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

// Exercise the real buffered-close error without a production I/O test hook.
struct DepsLogTest {
  static void Buffer(DepsLog& log) { log.flush_on_record_ = false; }
  static FILE* File(DepsLog& log) { return log.file_; }
  static bool Close(DepsLog& log) { return log.CloseForWrite(); }
};

namespace {
std::string Contents(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return {(std::istreambuf_iterator<char>(file)), {}};
}
}

TEST(DepsRecompact, AppendedRecordsAreVisibleBeforeClose) {
  ScopedFilePath file("DepsRecompact-visible");
  State state;
  DepsLog log;
  std::string err;
  ASSERT_TRUE(log.OpenForWrite(file.path(), &err));
  Node* out = state.GetNode("out", 0);
  Node* first = state.GetNode("first.h", 0);
  Node* second = state.GetNode("second.h", 0);
  for (int update = 0; update < 2; ++update) {
    const std::vector<Node*> inputs = update ? std::vector<Node*>{second, first, second}
                                            : std::vector<Node*>{first};
    ASSERT_TRUE(log.RecordDeps(out, 100 + update, inputs));
    // Another process must be able to use a finished command's deps even if
    // the appending process is subsequently interrupted without closing it.
    State reader_state;
    DepsLog reader;
    ASSERT_EQ(LOAD_SUCCESS, reader.Load(file.path(), &reader_state, &err));
    ASSERT_EQ("", err);
    const auto* deps = reader.GetDeps(reader_state.LookupNode("out"));
    ASSERT_NE(nullptr, deps);
    EXPECT_EQ(100 + update, deps->mtime);
    ASSERT_EQ(inputs.size(), deps->node_count);
    for (size_t i = 0; i < inputs.size(); ++i)
      EXPECT_EQ(inputs[i]->path(), deps->nodes[i]->path());
  }
}

TEST(DepsRecompact, BufferedRewritePreservesWireBytesAcrossMultipleFlushes) {
  ScopedFilePath file("DepsRecompact-buffers");
  ScopedFilePath temporary(file.path() + ".recompact");
  State state;
  std::string manifest = "rule cc\n  command = cc\n  deps = gcc\n";
  for (int i = 0; i < 128; ++i)
    manifest += "build out" + std::to_string(i) + ": cc\n";
  AssertParse(&state, manifest.c_str());
  DepsLog log;
  std::string err;
  ASSERT_TRUE(log.OpenForWrite(file.path(), &err));
  std::vector<Node*> headers;
  for (int i = 0; i < 5000; ++i)
    headers.push_back(state.GetNode("header" + std::to_string(i) + ".h", 0));
  std::vector<Node*> inputs(4096);
  for (int i = 0; i < 128; ++i) {
    for (size_t j = 0; j < inputs.size(); ++j)
      inputs[j] = headers[(i * 37 + j) % headers.size()];
    ASSERT_TRUE(log.RecordDeps(state.LookupNode("out" + std::to_string(i)),
                               1000 + i, inputs));
  }
  log.Close();
  const std::string expected = Contents(file.path());
  ASSERT_GT(expected.size(), 4u * (512u << 10));
  for (int pass = 0; pass < 2; ++pass) {
    ASSERT_TRUE(log.Recompact(file.path(), &err)) << err;
    EXPECT_EQ(expected, Contents(file.path()));
  }
  State reader_state;
  DepsLog reader;
  ASSERT_EQ(LOAD_SUCCESS, reader.Load(file.path(), &reader_state, &err));
  ASSERT_EQ("", err);
  for (int i = 0; i < 128; ++i) {
    const auto* deps = reader.GetDeps(reader_state.LookupNode("out" + std::to_string(i)));
    ASSERT_NE(nullptr, deps);
    EXPECT_EQ(1000 + i, deps->mtime);
    ASSERT_EQ(4096, deps->node_count);
    for (size_t j : {0u, 1u, 2048u, 4095u})
      EXPECT_EQ(headers[(i * 37 + j) % headers.size()]->path(), deps->nodes[j]->path());
  }
}

TEST(DepsRecompact, CheckedCloseReportsDelayedWriteFailureAndDoesNotReopen) {
  ScopedFilePath file("DepsRecompact-close-error");
  State state;
  DepsLog log;
  DepsLogTest::Buffer(log);
  std::string err;
  ASSERT_TRUE(log.OpenForWrite(file.path(), &err));
  ASSERT_TRUE(log.RecordDeps(state.GetNode("out", 0), 1,
                             std::vector<Node*>{state.GetNode("in", 0)}));
  FILE* stream = DepsLogTest::File(log);
  ASSERT_NE(nullptr, stream);
  // The header was flushed on open; the buffered records have not reached the
  // OS yet. Invalidate the underlying descriptor so that the final write fails.
#ifdef _WIN32
  const bool invalidated = CloseHandle(
      reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)))) != FALSE;
#else
  const bool invalidated = close(fileno(stream)) == 0;
#endif
  errno = 0;
  const bool closed = DepsLogTest::Close(log);
  const int error = errno;
  EXPECT_TRUE(invalidated);
  EXPECT_FALSE(closed);
  EXPECT_NE(0, error);
  EXPECT_EQ(nullptr, DepsLogTest::File(log));
  EXPECT_TRUE(DepsLogTest::Close(log));
  EXPECT_EQ(std::string("# ninjadeps\n\x04\0\0\0", 16), Contents(file.path()));
}

TEST(DepsRecompact, AutomaticCompactionKeepsLaterAppendAndOtherOutputsValid) {
  ScopedFilePath file("DepsRecompact-automatic");
  ScopedFilePath temporary(file.path() + ".recompact");
  const char* manifest = "rule cc\n  command = cc\n  deps = gcc\n"
                         "build a: cc\nbuild b: cc\n";
  {
    State state;
    AssertParse(&state, manifest);
    DepsLog log;
    std::string err;
    ASSERT_TRUE(log.OpenForWrite(file.path(), &err));
    Node* header = state.GetNode("header.h", 0);
    ASSERT_TRUE(log.RecordDeps(state.GetNode("dead", 0), 1, std::vector<Node*>{header}));
    for (int i = 0; i < 1100; ++i)
      ASSERT_TRUE(log.RecordDeps(state.LookupNode("a"), i, std::vector<Node*>{header}));
    ASSERT_TRUE(log.RecordDeps(state.LookupNode("b"), 7, std::vector<Node*>{header, header}));
  }
  {
    State state;
    AssertParse(&state, manifest);
    DepsLog log;
    std::string err;
    ASSERT_EQ(LOAD_SUCCESS, log.Load(file.path(), &state, &err));
    ASSERT_EQ("", err);
    const size_t previous_size = Contents(file.path()).size();
    // Open triggers the history-ratio compaction without an explicit tool call.
    ASSERT_TRUE(log.OpenForWrite(file.path(), &err)) << err;
    EXPECT_LT(Contents(file.path()).size(), previous_size);
    EXPECT_EQ(-1, state.LookupNode("dead")->id());
    Node* first = state.GetNode("first.h", 0);
    Node* header = state.LookupNode("header.h");
    ASSERT_TRUE(log.RecordDeps(state.LookupNode("a"), 2000,
                               std::vector<Node*>{first, header, first}));
    const auto* other = log.GetDeps(state.LookupNode("b"));
    ASSERT_NE(nullptr, other);
    ASSERT_EQ(2, other->node_count);
    EXPECT_EQ(header, other->nodes[0]);
    EXPECT_EQ(header, other->nodes[1]);
  }
  State state;
  AssertParse(&state, manifest);
  DepsLog log;
  std::string err;
  ASSERT_EQ(LOAD_SUCCESS, log.Load(file.path(), &state, &err));
  ASSERT_EQ("", err);
  const auto* deps = log.GetDeps(state.LookupNode("a"));
  ASSERT_NE(nullptr, deps);
  EXPECT_EQ(2000, deps->mtime);
  ASSERT_EQ(3, deps->node_count);
  EXPECT_EQ("first.h", deps->nodes[0]->path());
  EXPECT_EQ("header.h", deps->nodes[1]->path());
  EXPECT_EQ("first.h", deps->nodes[2]->path());
  EXPECT_EQ(7, log.GetDeps(state.LookupNode("b"))->mtime);
}

#ifdef _WIN32
TEST(DepsRecompact, OpenAndReplaceFailuresPreserveOriginalRecordsAndNodeIds) {
  for (bool lock_original : {false, true}) {
    ScopedFilePath file(lock_original ? "DepsRecompact-replace-failure" : "DepsRecompact-open-failure");
    ScopedFilePath temporary(file.path() + ".recompact");
    State state;
    AssertParse(&state, "rule cc\n  command = cc\n  deps = gcc\nbuild out: cc\n");
    DepsLog log;
    std::string err;
    ASSERT_TRUE(log.OpenForWrite(file.path(), &err));
    Node* header = state.GetNode("header.h", 0);
    Node* out = state.LookupNode("out");
    ASSERT_TRUE(log.RecordDeps(state.GetNode("dead", 0), 1, std::vector<Node*>{header}));
    ASSERT_TRUE(log.RecordDeps(out, 2, std::vector<Node*>{header, header}));
    log.Close();
    const auto nodes = log.nodes();
    const auto* deps = log.GetDeps(out);
    const std::string expected = Contents(file.path());
    {
      struct HeldFile {
        HANDLE handle;
        ~HeldFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
      } held{CreateFileA(lock_original ? file.c_str() : temporary.c_str(),
                         GENERIC_READ, FILE_SHARE_READ, nullptr,
                         lock_original ? OPEN_EXISTING : CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr)};
      ASSERT_NE(INVALID_HANDLE_VALUE, held.handle);
      EXPECT_FALSE(log.Recompact(file.path(), &err));
      EXPECT_FALSE(err.empty());
      EXPECT_EQ(expected, Contents(file.path()));
      EXPECT_EQ(nodes, log.nodes());
      for (size_t id = 0; id < nodes.size(); ++id)
        EXPECT_EQ(id, nodes[id]->id());
      ASSERT_EQ(deps, log.GetDeps(out));
      ASSERT_EQ(2, deps->node_count);
      EXPECT_EQ(header, deps->nodes[0]);
      EXPECT_EQ(header, deps->nodes[1]);
    }
    err.clear();
    ASSERT_TRUE(log.Recompact(file.path(), &err)) << err;
    EXPECT_EQ(0, out->id());
    EXPECT_EQ(2u, log.nodes().size());
    EXPECT_EQ(header, log.GetDeps(out)->nodes[1]);
  }
}
#endif
