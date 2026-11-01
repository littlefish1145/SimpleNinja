#include "deps_log.h"
#include "graph.h"
#include "state.h"
#include "test.h"

#include <cstring>
#include <fstream>

namespace {
void Word(std::string& data, uint32_t value) {
  data.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

std::string Header() {
  std::string data = "# ninjadeps\n";
  Word(data, 4);
  return data;
}

void Path(std::string& data, std::string name, uint32_t id, bool pad = true) {
  if (pad)
    name.append((-name.size()) % 4, '\0');
  Word(data, static_cast<uint32_t>(name.size() + 4));
  data += name;
  Word(data, ~id);
}

void Write(const char* path, const std::string& data) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(data.data(), data.size());
  ASSERT_TRUE(file.good());
}
}  // namespace

TEST(DepsReader, SmallAndLargeRecordsCrossRefillBoundaries) {
  ScopedFilePath file("DepsReader-boundaries");
  std::string data = Header();
  const uint32_t count = 5000;
  for (uint32_t i = 0; i < count; ++i)
    Path(data, "long/varied/path/" + std::to_string(i), i);
  // Alternate sizes, including records larger than the initial read buffer.
  for (uint32_t i = 0; i < 1500; ++i) {
    const uint32_t deps = i % 47 == 0 ? 20000 : i % 100;
    Word(data, 0x80000000 | (12 + 4 * deps));
    Word(data, i);
    Word(data, i + 1);
    Word(data, 3);
    for (uint32_t j = 0; j < deps; ++j)
      Word(data, (i + j + 1) % count);
  }
  Write(file.c_str(), data);
  State state;
  DepsLog log;
  std::string err;
  ASSERT_EQ(LOAD_SUCCESS, log.Load(file.path(), &state, &err));
  ASSERT_EQ("", err);
  ASSERT_EQ(count, log.nodes().size());
  for (uint32_t i = 0; i < 1500; ++i) {
    const auto* deps = log.GetDeps(log.nodes()[i]);
    ASSERT_NE(nullptr, deps);
    EXPECT_EQ((int64_t(3) << 32) | (i + 1), deps->mtime);
    const uint32_t expected = i % 47 == 0 ? 20000 : i % 100;
    ASSERT_EQ(expected, deps->node_count);
    for (uint32_t j = 0; j < expected; ++j)
      ASSERT_EQ(log.nodes()[(i + j + 1) % count], deps->nodes[j]);
  }
}

TEST(DepsReader, InvalidLengthsAndIdsRecoverWithoutOutOfBoundsReads) {
  ScopedFilePath file("DepsReader-corrupt");
  std::string prefix = Header();
  Path(prefix, "out", 0);
  Path(prefix, "in", 1);
  std::vector<std::string> corrupt;
  for (uint32_t size : {0u, 4u, 8u}) {
    std::string record;
    Word(record, 0x80000000 | size);
    record.append(size, '\0');
    corrupt.push_back(record);
  }
  for (uint32_t id : {2u, 0xffffffffu}) {
    std::string record;
    Word(record, 0x80000010);
    Word(record, 0);
    Word(record, 1);
    Word(record, 0);
    Word(record, id);
    corrupt.push_back(record);
  }
  // Empty padded paths, including lengths smaller than a word.
  for (uint32_t size : {1u, 4u, 5u, 6u, 7u}) {
    std::string record;
    Word(record, size);
    record.append(size, '\0');
    corrupt.push_back(record);
  }
  for (const auto& suffix : corrupt) {
    Write(file.c_str(), prefix + suffix);
    State state;
    DepsLog log;
    std::string err;
    ASSERT_EQ(LOAD_SUCCESS, log.Load(file.path(), &state, &err));
    EXPECT_EQ("premature end of file; recovering", err);
    EXPECT_EQ(2u, log.nodes().size());
    std::ifstream recovered(file.c_str(), std::ios::binary);
    std::string contents((std::istreambuf_iterator<char>(recovered)), {});
    EXPECT_EQ(prefix, contents);
  }
}

TEST(DepsReader, TruncatedHeadersKeepPartialVersionDiagnostics) {
  ScopedFilePath file("DepsReader-header-cuts");
  for (uint32_t version : {1u, 4u, 9u}) {
    for (bool signature : {false, true}) {
      std::string complete = Header().substr(0, 12);
      if (!signature) complete[0] = '!';
      Word(complete, version);
      for (size_t length = 0; length <= complete.size(); ++length) {
        SCOPED_TRACE(length);
        Write(file.c_str(), complete.substr(0, length));
        State state;
        DepsLog log;
        std::string err;
        ASSERT_EQ(LOAD_SUCCESS, log.Load(file.path(), &state, &err));
        // fread copies an incomplete version element into the initialized
        // word even when it returns zero complete elements.
        uint32_t partial_version = 0;
        if (length > 12)
          memcpy(&partial_version, complete.data() + 12, length - 12);
        const bool valid = signature && version == 4 && length == 16;
        EXPECT_EQ(valid ? "" : partial_version == 1
                                  ? "deps log version change; rebuilding"
                                  : "bad deps log signature or version; starting over", err);
        EXPECT_TRUE(log.nodes().empty());
      }
    }
  }
}

TEST(DepsReader, CrossingSizeFieldsAndTruncatedPayloadsRecoverAtExactPrefix) {
  ScopedFilePath file("DepsReader-block-cuts");
  for (size_t block : {size_t(64 << 10), size_t(128 << 10)}) {
    for (int delta = -3; delta <= 3; ++delta) {
      const size_t start = block + delta;
      SCOPED_TRACE(start);
      std::string prefix = Header();
      Path(prefix, "out", 0);
      Path(prefix, "in", 1);
      Path(prefix, std::string(start - prefix.size() - 8, 'p'), 2, false);
      ASSERT_EQ(start, prefix.size());
      std::string record;
      Word(record, 0x8000010c);  // 12-byte header plus 64 input IDs.
      Word(record, 0);
      Word(record, 1234);
      Word(record, 0);
      for (int i = 0; i < 64; ++i) Word(record, 1);
      for (size_t cut : {size_t(0), size_t(1), size_t(2), size_t(3), size_t(4),
                         size_t(5), size_t(15), record.size() - 1, record.size()}) {
        SCOPED_TRACE(cut);
        const std::string data = prefix + record.substr(0, cut);
        Write(file.c_str(), data);
        State state;
        DepsLog log;
        std::string err;
        ASSERT_EQ(LOAD_SUCCESS, log.Load(file.path(), &state, &err));
        const bool incomplete = cut >= 4 && cut < record.size();
        EXPECT_EQ(incomplete ? "premature end of file; recovering" : "", err);
        std::ifstream recovered(file.c_str(), std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(recovered)), {});
        EXPECT_EQ(incomplete ? prefix : data, contents);
        ASSERT_EQ(3u, log.nodes().size());
        const auto* deps = log.GetDeps(state.LookupNode("out"));
        if (cut == record.size()) {
          ASSERT_NE(nullptr, deps);
          ASSERT_EQ(64, deps->node_count);
          EXPECT_EQ(1234, deps->mtime);
          EXPECT_EQ(state.LookupNode("in"), deps->nodes[63]);
        } else {
          EXPECT_EQ(nullptr, deps);
        }
      }
    }
  }
}

TEST(DepsReader, LargestRecordAtUnalignedBlockBoundary) {
  ScopedFilePath file("DepsReader-largest");
  std::string data = Header();
  Path(data, "out", 0);
  Path(data, "in", 1);
  Path(data, std::string((64 << 10) - 3 - data.size() - 8, 'p'), 2, false);
  Word(data, 0x8007fffc);
  Word(data, 0);
  Word(data, 0x55667788);
  Word(data, 0x11223344);
  const int count = 131068;
  for (int i = 0; i < count; ++i) Word(data, 1);
  Write(file.c_str(), data);
  State state;
  DepsLog log;
  std::string err;
  ASSERT_EQ(LOAD_SUCCESS, log.Load(file.path(), &state, &err));
  ASSERT_EQ("", err);
  const auto* deps = log.GetDeps(state.LookupNode("out"));
  ASSERT_NE(nullptr, deps);
  ASSERT_EQ(count, deps->node_count);
  EXPECT_EQ(INT64_C(0x1122334455667788), deps->mtime);
  for (int i = 0; i < count; ++i)
    ASSERT_EQ(state.LookupNode("in"), deps->nodes[i]);
}

TEST(DepsReader, UnalignedRecordAndIncompleteFinalSizeRetainExistingSemantics) {
  ScopedFilePath file("DepsReader-unaligned");
  std::string data = Header();
  Path(data, "out", 0, false);
  Path(data, "in", 1, false);
  Word(data, 0x80000010);
  Word(data, 0);
  Word(data, 1234);
  Word(data, 0);
  Word(data, 1);
  // An incomplete next size is ignored, as by Ninja's fread(size, 4, 1).
  data += "abc";
  Write(file.c_str(), data);
  State state;
  DepsLog log;
  std::string err;
  ASSERT_EQ(LOAD_SUCCESS, log.Load(file.path(), &state, &err));
  ASSERT_EQ("", err);
  const auto* deps = log.GetDeps(state.LookupNode("out"));
  ASSERT_NE(nullptr, deps);
  ASSERT_EQ(1, deps->node_count);
  EXPECT_EQ(1234, deps->mtime);
  EXPECT_EQ(state.LookupNode("in"), deps->nodes[0]);
  std::ifstream original(file.c_str(), std::ios::binary);
  std::string contents((std::istreambuf_iterator<char>(original)), {});
  EXPECT_EQ(data, contents);
}

TEST(DepsReader, WriterPreservesExactRecordBytesAndPadding) {
  ScopedFilePath file("DepsReader-wire-format");
  State state;
  std::vector<Node*> inputs;
  const std::vector<std::string> names = {"a", "bb", "ccc", "dddd", "eeeee"};
  for (const auto& name : names)
    inputs.push_back(state.GetNode(name, 0));
  Node* output = state.GetNode("out.o", 0);
  std::string err;
  DepsLog writer;
  ASSERT_TRUE(writer.OpenForWrite(file.path(), &err));
  const int64_t mtime = INT64_C(0x1122334455667788);
  ASSERT_TRUE(writer.RecordDeps(output, mtime, inputs));
  // The second identical update must not append another record.
  ASSERT_TRUE(writer.RecordDeps(output, mtime, inputs));
  writer.Close();
  std::string expected = Header();
  Path(expected, "out.o", 0);
  for (uint32_t i = 0; i < names.size(); ++i)
    Path(expected, names[i], i + 1);
  Word(expected, 0x80000000 | (12 + 4 * static_cast<uint32_t>(names.size())));
  Word(expected, 0);
  Word(expected, 0x55667788);
  Word(expected, 0x11223344);
  for (uint32_t i = 0; i < names.size(); ++i)
    Word(expected, i + 1);
  std::ifstream recorded(file.c_str(), std::ios::binary);
  std::string contents((std::istreambuf_iterator<char>(recorded)), {});
  EXPECT_EQ(expected, contents);
}

TEST(DepsCompact, NodeTableGrowthUpdatesAndRepeatedCompactionPreserveOrder) {
  ScopedFilePath file("DepsCompact-rebind");
  const char* manifest = "rule cc\n  command = cc $in -o $out\n  deps = gcc\n"
                         "build a: cc\nbuild b: cc\nbuild empty: cc\n";
  State state;
  AssertParse(&state, manifest);
  DepsLog log;
  std::string err;
  ASSERT_TRUE(log.OpenForWrite(file.path(), &err));
  Node* first = state.GetNode("first.h", 0);
  Node* second = state.GetNode("second.h", 0);
  std::vector<Node*> inputs = {first, second, first};
  // Dead records precede live ones so compaction must change the live IDs.
  ASSERT_TRUE(log.RecordDeps(state.GetNode("dead", 0), 1, inputs));
  Node* a = state.LookupNode("a");
  Node* b = state.LookupNode("b");
  ASSERT_TRUE(log.RecordDeps(a, 2, inputs));
  const auto* saved_a = log.GetDeps(a);
  std::vector<Node*> reverse = {second, first};
  ASSERT_TRUE(log.RecordDeps(b, 3, reverse));
  std::vector<Node*> none;
  ASSERT_TRUE(log.RecordDeps(state.LookupNode("empty"), 4, none));
  for (int i = 0; i < 4000; ++i)
    ASSERT_TRUE(log.RecordDeps(state.GetNode("unused" + std::to_string(i), 0), 1, none));
  // Growing nodes_ must not leave a view of its former allocation behind.
  ASSERT_EQ(saved_a, log.GetDeps(a));
  EXPECT_EQ(first, saved_a->nodes[0]);
  EXPECT_EQ(second, saved_a->nodes[1]);
  EXPECT_EQ(first, saved_a->nodes[2]);
  ASSERT_GT(a->id(), 0);
  ASSERT_TRUE(log.Recompact(file.path(), &err));
  EXPECT_EQ(0, a->id());
  EXPECT_EQ(5u, log.nodes().size());
  ASSERT_EQ(3, log.GetDeps(a)->node_count);
  EXPECT_EQ(first, log.GetDeps(a)->nodes[0]);
  EXPECT_EQ(second, log.GetDeps(a)->nodes[1]);
  EXPECT_EQ(first, log.GetDeps(a)->nodes[2]);
  EXPECT_EQ(second, log.GetDeps(b)->nodes[0]);
  EXPECT_EQ(0, log.GetDeps(state.LookupNode("empty"))->node_count);

  // Append new IDs after the swap, replace an existing record, then compact
  // again. A compact record must resolve through its current owner's table.
  ASSERT_TRUE(log.OpenForWrite(file.path(), &err));
  Node* third = state.GetNode("third.h", 0);
  inputs = {third, second, first, third};
  ASSERT_TRUE(log.RecordDeps(a, 5, inputs));
  for (int i = 0; i < 4000; ++i)
    ASSERT_TRUE(log.RecordDeps(state.GetNode("later" + std::to_string(i), 0), 1, none));
  EXPECT_EQ(second, log.GetDeps(b)->nodes[0]);
  EXPECT_EQ(first, log.GetDeps(b)->nodes[1]);
  ASSERT_TRUE(log.Recompact(file.path(), &err));
  EXPECT_EQ(6u, log.nodes().size());
  for (size_t i = 0; i < inputs.size(); ++i)
    EXPECT_EQ(inputs[i], log.GetDeps(a)->nodes[i]);
  EXPECT_EQ(second, log.GetDeps(b)->nodes[0]);
  EXPECT_EQ(first, log.GetDeps(b)->nodes[1]);

  State reloaded_state;
  AssertParse(&reloaded_state, manifest);
  DepsLog reloaded;
  ASSERT_EQ(LOAD_SUCCESS, reloaded.Load(file.path(), &reloaded_state, &err));
  ASSERT_EQ("", err);
  const auto* restored = reloaded.GetDeps(reloaded_state.LookupNode("a"));
  ASSERT_NE(nullptr, restored);
  ASSERT_EQ(4, restored->node_count);
  EXPECT_EQ(5, restored->mtime);
  for (size_t i = 0; i < inputs.size(); ++i)
    EXPECT_EQ(inputs[i]->path(), restored->nodes[i]->path());
}
