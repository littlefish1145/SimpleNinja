// A native subprocess workload, deliberately independent of the build engine.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#include <unistd.h>
#endif

static uint64_t Now() {
#ifdef _WIN32
  LARGE_INTEGER value;
  QueryPerformanceCounter(&value);
  return value.QuadPart;
#else
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

int main(int argc, char** argv) {
  const uint64_t start = Now();
  if (argc == 3 && std::strcmp(argv[1], "--emit") == 0) {
    uint64_t remaining = std::strtoull(argv[2], nullptr, 10);
    char data[4096];
    std::memset(data, 'A', sizeof(data));
    while (remaining) {
      const size_t length = remaining < sizeof(data) ? remaining : sizeof(data);
      if (std::fwrite(data, 1, length, stdout) != length) return 6;
      remaining -= length;
    }
    return std::fflush(stdout) == 0 ? 0 : 7;
  }
#ifdef _WIN32
  if (argc == 3 && std::strcmp(argv[1], "--inherited-event") == 0)
    return SetEvent(reinterpret_cast<HANDLE>(std::strtoull(argv[2], nullptr, 10))) ? 0 : 5;
#endif
  if (argc != 4) return 2;
  const uint64_t microseconds = std::strtoull(argv[2], nullptr, 10);
  const uint64_t index = std::strtoull(argv[3], nullptr, 10);
#ifdef _WIN32
  LARGE_INTEGER hz;
  QueryPerformanceFrequency(&hz);
  uint64_t frequency = hz.QuadPart;
  uint32_t pid = GetCurrentProcessId();
#else
  uint64_t frequency = 1000000000;
  uint32_t pid = getpid();
#endif
  FILE* output = std::fopen(argv[1], "wb");
  if (!output) return 3;
  uint64_t checksum = index + 1;
  if (microseconds) {
    const uint64_t deadline = start + microseconds * frequency / 1000000;
    do {
      for (int i = 0; i < 64; ++i)
        checksum = (checksum ^ (checksum >> 11)) * UINT64_C(0xd6e8feb86659fd93);
    } while (Now() < deadline);
  }
  const uint64_t end = Now();
  // uint64 start, end, index, checksum, frequency; uint32 pid, magic.
  const uint64_t words[] = {start, end, index, checksum, frequency};
  const uint32_t tail[] = {pid, UINT32_C(0x534e4a42)};
  const bool success = std::fwrite(words, sizeof(words), 1, output) == 1 &&
                       std::fwrite(tail, sizeof(tail), 1, output) == 1;
  const int close_status = std::fclose(output);
  return success && close_status == 0 ? 0 : 4;
}
