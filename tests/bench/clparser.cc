// Compare compiler-output parsing against the unmodified reference engine.
#include "clparser.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3)
    return 2;
  std::ifstream input(argv[1], std::ios::binary);
  if (!input)
    return 3;
  std::string text((std::istreambuf_iterator<char>(input)), {});
  CLParser parser;
  std::string output, error;
  const auto start = std::chrono::steady_clock::now();
  if (!parser.Parse(text, argc == 3 ? argv[2] : "", &output, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 4;
  }
  const auto end = std::chrono::steady_clock::now();
  uint64_t hash = UINT64_C(14695981039346656037);
  const auto add = [&hash](const std::string& value) {
    for (unsigned char c : value) {
      hash ^= c;
      hash *= UINT64_C(1099511628211);
    }
    hash ^= 255;
    hash *= UINT64_C(1099511628211);
  };
  add(output);
  for (const auto& path : parser.includes_)
    add(path);
  std::printf("{\"parse_ms\":%.6f,\"includes\":%zu,\"filtered_bytes\":%zu,\"hash\":\"%016llx\"}\n",
              std::chrono::duration<double, std::milli>(end - start).count(),
              parser.includes_.size(), output.size(), static_cast<unsigned long long>(hash));
}
