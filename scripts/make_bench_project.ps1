# Generate a wide project: many independent translation units that share one
# header, plus a main that depends on all of them.  That gives a scheduler real
# parallelism to exploit, a real ordering constraint, and a header that the msvc
# deps path must record in the deps log.
param(
  [string]$Project = "$env:TEMP\sninja_bench",
  [int]$Units = 120
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path "$Project\src" | Out-Null

$declares = (0..($Units - 1) | ForEach-Object { "int Unit${_}_Value();" }) -join "`n"
$calls = (0..($Units - 1) | ForEach-Object { "  total += Unit${_}_Value();" }) -join "`n"

# One shared header, declaring every unit.
@"
#pragma once
$declares
"@ | Set-Content -Path "$Project\src\bench.h" -NoNewline

# Collect the unit paths in a plain .NET list: a PowerShell array would coerce
# "src/unit7.cpp" to an int, which throws for any index >= 10.
$unitPaths = [System.Collections.Generic.List[string]]::new()
for ($i = 0; $i -lt $Units; $i++) {
  $unitPaths.Add("src/unit$i.cpp")
  @"
#include "bench.h"
int Unit${i}_Value() { return $i * 3; }
"@ | Set-Content -Path "$Project\src\unit$i.cpp" -NoNewline
}

@"
#include "bench.h"
#include <cstdio>
int main() {
  int total = 0;
$calls
  printf("%d\n", total);
  return 0;
}
"@ | Set-Content -Path "$Project\src\main.cpp" -NoNewline

@"
cmake_minimum_required(VERSION 3.20)
project(sninja_bench CXX)
add_library(bench STATIC $($unitPaths -join ' '))
target_include_directories(bench PUBLIC src)
add_executable(bench_exe src/main.cpp)
target_link_libraries(bench_exe PRIVATE bench)
"@ | Set-Content -Path "$Project\CMakeLists.txt" -NoNewline

"created $Project with $Units units"
