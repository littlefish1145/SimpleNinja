#include "build_log.h"
#include "test.h"

#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>

namespace {
void WriteLog(const ScopedFilePath& path, const std::string& contents) {
  std::ofstream file(path.path(), std::ios::binary | std::ios::trunc);
  file.write(contents.data(), contents.size());
  ASSERT_TRUE(file.good());
}

std::string Hex(uint64_t value) {
  char buffer[32];
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, 16);
  return {buffer, result.ptr};
}

struct AllLive : BuildLogUser {
  bool IsPathDead(StringPiece) const override { return false; }
};
}

TEST(BuildLogReader, NumericFieldsKeepLegacySpellingsAndOverflowResults) {
  ScopedFilePath file("BuildLogReader-numbers");
  const std::vector<std::string> spellings = {
      "", "0", "-0", "+17", " 23", "\v31", "44suffix", "77\r", "-19",
      "000012", "2147483647", "2147483648", "-2147483648", "-2147483649",
      "9223372036854775807", "-9223372036854775808", "9223372036854775808",
      "-9223372036854775809", "999999999999999999999999999999999",
      "0x123", "ff", "0XfF", "-ff", "ffffffffffffffff", "10000000000000000",
      std::string("12\0ignored", 10)};
  std::vector<std::array<std::string, 4>> fields;
  for (const auto& spelling : spellings) {
    for (size_t column = 0; column < 4; ++column) {
      auto values = std::array<std::string, 4>{"1", "2", "3", "ff"};
      values[column] = spelling;
      fields.push_back(std::move(values));
    }
  }
  std::string contents = "# ninja log v7\n";
  for (size_t i = 0; i < fields.size(); ++i) {
    const auto& row = fields[i];
    contents += row[0] + "\t" + row[1] + "\t" + row[2] + "\toutput" +
                std::to_string(i) + "\t" + row[3] + (i % 2 ? "\r\n" : "\n");
  }
  WriteLog(file, contents);
  BuildLog log;
  std::string err;
  ASSERT_EQ(LOAD_SUCCESS, log.Load(file.path(), &err));
  ASSERT_EQ("", err);
  ASSERT_EQ(fields.size(), log.entries().size());
  for (size_t i = 0; i < fields.size(); ++i) {
    SCOPED_TRACE(i);
    const auto* entry = log.LookupByOutput("output" + std::to_string(i));
    ASSERT_NE(nullptr, entry);
    EXPECT_EQ(std::atoi(fields[i][0].c_str()), entry->start_time);
    EXPECT_EQ(std::atoi(fields[i][1].c_str()), entry->end_time);
    EXPECT_EQ(std::strtoll(fields[i][2].c_str(), nullptr, 10), entry->mtime);
    EXPECT_EQ(strtoull(fields[i][3].c_str(), nullptr, 16), entry->command_hash);
  }
}

TEST(BuildLogReader, HistoryPathsOwnTheirKeysAcrossRefillsRehashAndRecompaction) {
  ScopedFilePath file("BuildLogReader-history");
  ScopedFilePath temporary(file.path() + ".recompact");
  constexpr int outputs = 3000;
  constexpr int history = 5;
  std::vector<std::string> paths;
  for (int i = 0; i < outputs; ++i)
    paths.push_back("objects/" + std::string(90, 'p') + "/source" + std::to_string(i) + ".obj");
  std::string contents = "# ninja log v7\n";
  for (int pass = 0; pass < history; ++pass) {
    for (int i = 0; i < outputs; ++i) {
      const int start = pass * outputs + i;
      const auto hash = UINT64_C(0x8123456780000000) + start;
      contents += std::to_string(start) + "\t" + std::to_string(start + 7) + "\t" +
                  std::to_string(INT64_C(1000000000000) + start) + "\t" + paths[i] + "\t" +
                  Hex(hash) + (pass % 2 ? "\r\n" : "\n");
    }
  }
  ASSERT_GT(contents.size(), 4u * (256u << 10));
  WriteLog(file, contents);
  BuildLog log;
  std::string err;
  ASSERT_EQ(LOAD_SUCCESS, log.Load(file.path(), &err));
  ASSERT_EQ("", err);
  ASSERT_EQ(outputs, log.entries().size());
  // Discard the original text before checking the map after all refills.
  contents.assign(2 << 20, 'x');
  for (int i = 0; i < outputs; ++i) {
    const auto* entry = log.LookupByOutput(paths[i]);
    ASSERT_NE(nullptr, entry);
    const int start = (history - 1) * outputs + i;
    EXPECT_EQ(paths[i], entry->output);
    EXPECT_EQ(start, entry->start_time);
    EXPECT_EQ(start + 7, entry->end_time);
    EXPECT_EQ(INT64_C(1000000000000) + start, entry->mtime);
    EXPECT_EQ(UINT64_C(0x8123456780000000) + start, entry->command_hash);
  }
  AllLive user;
  ASSERT_TRUE(log.Recompact(file.path(), user, &err)) << err;
  BuildLog reader;
  ASSERT_EQ(LOAD_SUCCESS, reader.Load(file.path(), &err));
  ASSERT_EQ("", err);
  ASSERT_EQ(outputs, reader.entries().size());
  for (const auto& path : paths) {
    ASSERT_NE(nullptr, reader.LookupByOutput(path));
    EXPECT_TRUE(*log.LookupByOutput(path) == *reader.LookupByOutput(path));
  }
}
