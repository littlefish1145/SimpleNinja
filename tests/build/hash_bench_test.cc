#include "build_log.h"

#include <stdio.h>
#include <time.h>

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace {

// Build a command line of roughly the size CMake emits for a real edge.
std::string MakeCommand(size_t seed) {
  std::string s = "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\"
                  "VC\\Tools\\MSVC\\14.44.35207\\bin\\Hostx64\\x64\\cl.exe /nologo "
                  "/TP /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG -MD "
                  "/showIncludes /FoCMakeFiles\\bench.dir\\src\\unit";
  s += std::to_string(seed);
  s += ".cpp.obj /FdCMakeFiles\\bench.dir\\bench.pdb /FS -c "
       "C:\\Users\\someone\\src\\sninja_bench\\src\\unit";
  s += std::to_string(seed);
  s += ".cpp";
  return s;
}

// How much wall clock does hashing the commands of a large build actually take?
// The answer decides whether a SIMD hash is worth writing, so it is measured
// rather than assumed.
TEST(BuildLogBenchmark, HashingRealisticCommandsIsCheap) {
  const int kEdges = 5000;
  std::vector<std::string> commands;
  commands.reserve(kEdges);
  size_t total_bytes = 0;
  for (int i = 0; i < kEdges; ++i) {
    commands.push_back(MakeCommand(i));
    total_bytes += commands.back().size();
  }

  // Warm up, then time a few passes and keep the best to reduce noise.
  uint64_t sink = 0;
  for (int i = 0; i < kEdges; ++i)
    sink += BuildLog::LogEntry::HashCommand(commands[i]);

  // clock() is far too coarse for a single pass, so time many of them.
  const int kPasses = 200;
  double best_ms = 1e30;
  clock_t start = clock();
  for (int pass = 0; pass < kPasses; ++pass) {
    for (int i = 0; i < kEdges; ++i)
      sink += BuildLog::LogEntry::HashCommand(commands[i]);
  }
  const double ms = 1000.0 * (double)(clock() - start) / CLOCKS_PER_SEC;
  if (ms < best_ms)
    best_ms = ms;
  const double per_build_ms = ms / kPasses;

  printf("[bench] %d commands (%zu bytes) per build: %.4f ms, %.2f GB/s\n",
         kEdges, total_bytes, per_build_ms,
         total_bytes / (per_build_ms / 1000.0) / 1e9);
  fflush(stdout);
  EXPECT_TRUE(sink != 0);  // keep the compiler from eliding the loop
}

}  // namespace
