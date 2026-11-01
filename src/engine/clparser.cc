// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "clparser.h"

#include <algorithm>
#include <assert.h>
#include <string.h>
#include <string_view>

#include "metrics.h"
#include "string_piece_util.h"

#ifdef _WIN32
#include "includes_normalize.h"
#include "string_piece.h"
#else
#include "util.h"
#endif

using namespace std;

namespace {

string_view ShowIncludesPath(string_view line, string_view prefix) {
  if (prefix.empty())
    prefix = "Note: including file: ";
  if (line.size() <= prefix.size() || !line.starts_with(prefix))
    return {};
  line.remove_prefix(prefix.size());
  while (!line.empty() && line.front() == ' ')
    line.remove_prefix(1);
  return line;
}

bool EndsWithInsensitive(string_view input, string_view suffix) {
  if (input.size() < suffix.size())
    return false;
  input.remove_prefix(input.size() - suffix.size());
  for (size_t i = 0; i < suffix.size(); ++i) {
    if (ToLowerASCII(input[i]) != suffix[i])
      return false;
  }
  return true;
}

bool IsInputFilename(string_view line) {
  return EndsWithInsensitive(line, ".c") || EndsWithInsensitive(line, ".cc") ||
         EndsWithInsensitive(line, ".cxx") || EndsWithInsensitive(line, ".cpp") ||
         EndsWithInsensitive(line, ".c++");
}

}  // anonymous namespace

// static
string CLParser::FilterShowIncludes(const string& line,
                                    const string& deps_prefix) {
  return string(ShowIncludesPath(line, deps_prefix));
}

// static
bool CLParser::IsSystemInclude(const string& path) {
  // TODO: this is a heuristic, perhaps there's a better way?
  const auto contains = [&path](string_view needle) {
    return search(path.begin(), path.end(), needle.begin(), needle.end(),
                  [](char a, char b) { return ToLowerASCII(a) == b; }) != path.end();
  };
  return contains("program files") || contains("microsoft visual studio");
}

// static
bool CLParser::FilterInputFilename(const string& line) {
  // TODO: other extensions, like .asm?
  return IsInputFilename(line);
}

// static
bool CLParser::Parse(const string& output, const string& deps_prefix,
                     string* filtered_output, string* err) {
  METRIC_RECORD("CLParser::Parse");

  // Loop over all lines in the output to process them.
  assert(&output != filtered_output);
  size_t start = 0;
  bool seen_show_includes = false;
#ifdef _WIN32
  IncludesNormalize normalizer(".");
#endif

  while (start < output.size()) {
    size_t end = output.find_first_of("\r\n", start);
    if (end == string::npos)
      end = output.size();
    string_view line(output.data() + start, end - start);

    string_view include = ShowIncludesPath(line, deps_prefix);
    if (!include.empty()) {
      seen_show_includes = true;
      string normalized;
#ifdef _WIN32
      if (!normalizer.Normalize(string(include), &normalized, err))
        return false;
#else
      // TODO: should this make the path relative to cwd?
      normalized.assign(include.data(), include.size());
      uint64_t slash_bits;
      CanonicalizePath(&normalized, &slash_bits);
#endif
      if (!IsSystemInclude(normalized))
        includes_.insert(std::move(normalized));
    } else if (!seen_show_includes && IsInputFilename(line)) {
      // Drop it.
      // TODO: if we support compiling multiple output files in a single
      // cl.exe invocation, we should stash the filename.
    } else {
      filtered_output->append(line.data(), line.size());
      filtered_output->append("\n");
    }

    if (end < output.size() && output[end] == '\r')
      ++end;
    if (end < output.size() && output[end] == '\n')
      ++end;
    start = end;
  }

  return true;
}
