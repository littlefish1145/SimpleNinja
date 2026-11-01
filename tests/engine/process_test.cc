#ifdef _WIN32
#include <gtest/gtest.h>
#include "subprocess.h"
#include "test.h"
#include "util.h"
#include <io.h>
#include <chrono>
#include <algorithm>
#include <set>
#include <string>

TEST(FileHandles, InternalStdioFilesAreNonInheritableFromCreation) {
  ScopedFilePath path("FileHandles-noninheritable");
  for (const char* mode : {"w", "wb", "a", "ab", "r", "rb"}) {
    FILE* file = FOpenNonInheritable(path.c_str(), mode);
    ASSERT_NE(nullptr, file) << mode;
    DWORD flags = HANDLE_FLAG_INHERIT;
    const BOOL okay = GetHandleInformation(
        reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file))), &flags);
    EXPECT_TRUE(okay) << mode;
    EXPECT_EQ(0u, flags & HANDLE_FLAG_INHERIT) << mode;
    EXPECT_EQ(0, fclose(file));
  }
}

TEST(AsyncLaunch, EmptyOutputAndMixedLaunchErrors) {
  for (bool async : {false, true}) {
    SubprocessSet processes;
    processes.SetAsyncLaunch(async);
    std::set<Subprocess*> failures;
    const int count = 128;
    for (int i = 0; i < count; ++i) {
      const bool fail = i % 7 == 0;
      auto* process = processes.Add(fail ? "sninja_nonexistent_program_71d513" : "cmd /c exit 0");
      if (fail) failures.insert(process);
    }
    int finished = 0;
    while (finished != count) {
      processes.DoWork();
      while (auto* process = processes.NextFinished()) {
        const bool fail = failures.erase(process) != 0;
        EXPECT_EQ(fail ? ExitFailure : ExitSuccess, process->Finish());
        EXPECT_EQ(fail ? "CreateProcess failed: The system cannot find the file specified.\n" : "",
                  process->GetOutput());
        delete process;
        ++finished;
      }
    }
    EXPECT_TRUE(failures.empty());
  }
}

#ifdef SNINJA_JOB_PATH
TEST(AsyncLaunch, PreservesParentInheritableHandleValues) {
  SECURITY_ATTRIBUTES attributes = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE event = CreateEventA(&attributes, TRUE, FALSE, nullptr);
  ASSERT_NE(nullptr, event);
  SubprocessSet processes;
  processes.SetAsyncLaunch(true);
  for (int i = 0; i < 16; ++i) {
    std::string command = "\"" SNINJA_JOB_PATH "\" --inherited-event " +
        std::to_string(reinterpret_cast<uintptr_t>(event));
    processes.Add(std::move(command));
  }
  int finished = 0;
  while (finished != 16) {
    processes.DoWork();
    while (auto* child = processes.NextFinished()) {
      EXPECT_EQ(ExitSuccess, child->Finish());
      delete child;
      ++finished;
    }
  }
  EXPECT_EQ(WAIT_OBJECT_0, WaitForSingleObject(event, 0));
  CloseHandle(event);
}

TEST(AsyncLaunch, CancelsQueuedCommandsAndReusesCompletionPort) {
  SubprocessSet processes;
  processes.SetAsyncLaunch(true);
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 128; ++i)
    processes.Add("\"" SNINJA_JOB_PATH "\" cancelled_job" + std::to_string(i) + " 5000000 0");
  processes.Clear();
  EXPECT_TRUE(processes.running_.empty());
  // Waiting for a five-second child naturally would violate this bound.
  EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 4.0);
  processes.Add("cmd /c echo reused");
  while (!processes.HasFinished()) processes.DoWork();
  auto* child = processes.NextFinished();
  EXPECT_EQ(ExitSuccess, child->Finish());
  EXPECT_EQ("reused\r\n", child->GetOutput());
  delete child;
}

TEST(AsyncLaunch, CancelsActiveOverlappedReadsAndDrainsRetiredContexts) {
  SubprocessSet processes;
  processes.SetAsyncLaunch(true);
  for (int cycle = 0; cycle < 8; ++cycle) {
    const auto start = std::chrono::steady_clock::now();
    processes.Add("\"" SNINJA_JOB_PATH "\" cancelled_active" + std::to_string(cycle) + " 5000000 0");
    processes.Add("\"" SNINJA_JOB_PATH "\" cancelled_second" + std::to_string(cycle) + " 5000000 1");
    // One synchronous connect, plus an asynchronous connect and publication.
    // Consuming all three arms both reads without waiting for child output.
    processes.DoWork();
    processes.DoWork();
    processes.DoWork();
    processes.Clear();
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), 4.0);
    processes.Add("cmd /c echo next");
    while (!processes.HasFinished()) processes.DoWork();
    auto* child = processes.NextFinished();
    EXPECT_EQ(ExitSuccess, child->Finish());
    EXPECT_EQ("next\r\n", child->GetOutput());
    delete child;
  }
}

TEST(AsyncLaunch, CapturesConcurrentOutputLargerThanPipeBuffers) {
  SubprocessSet processes;
  processes.SetAsyncLaunch(true);
  for (int i = 0; i < 16; ++i)
    processes.Add("\"" SNINJA_JOB_PATH "\" --emit 262144");
  int finished = 0;
  while (finished != 16) {
    processes.DoWork();
    while (auto* child = processes.NextFinished()) {
      EXPECT_EQ(ExitSuccess, child->Finish());
      EXPECT_EQ(262144u, child->GetOutput().size());
      EXPECT_TRUE(std::all_of(child->GetOutput().begin(), child->GetOutput().end(),
                             [](char c) { return c == 'A'; }));
      delete child;
      ++finished;
    }
  }
}
#endif

TEST(ProcessHandles, RepeatedBatchesCloseChildPipesAndSharedNullInput) {
  DWORD warmed = 0, after;
  {
    SubprocessSet processes;
    for (int batch = 0; batch < 16; ++batch) {
      for (int i = 0; i < 8; ++i)
        ASSERT_NE(nullptr, processes.Add("cmd /c echo captured"));
      int finished = 0;
      while (finished < 8) {
        processes.DoWork();
        while (auto* child = processes.NextFinished()) {
          EXPECT_EQ(ExitSuccess, child->Finish());
          EXPECT_EQ("captured\r\n", child->GetOutput());
          delete child;
          ++finished;
        }
      }
      EXPECT_TRUE(processes.running_.empty());
      // The first CreateProcess lazily initializes CRT/ASan/console state.
      if (batch == 0)
        ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &warmed));
    }
  }
  ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &after));
  EXPECT_LE(after, warmed + 2);
}
#endif
