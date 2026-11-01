// Test runner for the shim in tests/gtest/gtest.h.
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace sninja_testing {

bool& CurrentTestFailed() {
  static bool failed = false;
  return failed;
}

void ReportFailure(const char* file, int line, const std::string& message) {
  CurrentTestFailed() = true;
  std::cout << file << ":" << line << ": Failure\n  " << message << "\n";
  std::cout.flush();
}

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

bool RegisterTest(const char* suite, const char* name, const char* file,
                  int line, Test* (*create)()) {
  Registry().push_back(TestCase{suite, name, file, line, create});
  return true;
}

bool Test::Run() {
  CurrentTestFailed() = false;
  TestBody();
  return !CurrentTestFailed();
}

namespace {

/// Glob-style matcher supporting '*' and '?', as googletest does.
bool PatternMatches(const char* pattern, const char* str) {
  const char* star = nullptr;
  const char* after_star = nullptr;
  while (*str) {
    if (*pattern == '?' || *pattern == *str) {
      ++pattern;
      ++str;
    } else if (*pattern == '*') {
      star = pattern++;
      after_star = str;
    } else if (star) {
      pattern = star + 1;
      str = ++after_star;
    } else {
      return false;
    }
  }
  while (*pattern == '*')
    ++pattern;
  return *pattern == '\0';
}

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> parts;
  std::string current;
  for (char c : s) {
    if (c == sep) {
      parts.push_back(current);
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  parts.push_back(current);
  return parts;
}

struct Filter {
  std::vector<std::string> positive;
  std::vector<std::string> negative;
  bool matches(const std::string& name) const {
    bool ok = positive.empty();
    for (const std::string& p : positive) {
      if (PatternMatches(p.c_str(), name.c_str())) {
        ok = true;
        break;
      }
    }
    if (!ok)
      return false;
    for (const std::string& p : negative) {
      if (PatternMatches(p.c_str(), name.c_str()))
        return false;
    }
    return true;
  }
};

}  // namespace
}  // namespace sninja_testing

int main(int argc, char** argv) {
  using namespace sninja_testing;

  Filter filter;
  std::string list_tests;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg.rfind("--gtest_filter=", 0) == 0) {
      std::string spec = arg.substr(strlen("--gtest_filter="));
      size_t dash = spec.find('-');
      if (dash == std::string::npos) {
        filter.positive = Split(spec, ':');
      } else {
        filter.positive = Split(spec.substr(0, dash), ':');
        std::string neg = spec.substr(dash + 1);
        if (!neg.empty())
          filter.negative = Split(neg, ':');
      }
    } else if (arg == "--gtest_list_tests") {
      list_tests = "yes";
    }
  }

  int total = 0, failed = 0;
  for (const TestCase& test : Registry()) {
    std::string full_name = test.suite + "." + test.name;
    if (!filter.matches(full_name))
      continue;
    ++total;
    if (!list_tests.empty()) {
      std::cout << full_name << "\n";
      continue;
    }
    std::cout << "[ RUN      ] " << full_name << "\n";
    Test* instance = test.create();
    bool ok = instance->Run();
    delete instance;
    std::cout << (ok ? "[       OK ] " : "[  FAILED  ] ") << full_name << "\n";
    if (!ok)
      ++failed;
  }

  if (list_tests.empty()) {
    std::cout << "[==========] " << total << " test(s) ran.\n";
    std::cout << "[  PASSED  ] " << (total - failed) << " test(s).\n";
    if (failed)
      std::cout << "[  FAILED  ] " << failed << " test(s).\n";
  }
  return failed ? 1 : 0;
}
