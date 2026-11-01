# Create the sample C++ project that the scheduler tests build.
param([string]$Project = "$env:TEMP\sninja_proj")

New-Item -ItemType Directory -Force -Path "$Project\src" | Out-Null

@'
#pragma once
inline int shared() { return 42; }
'@ | Set-Content -Path "$Project\src\common.h" -NoNewline

@'
#include "common.h"
int a() { return shared() + 1; }
'@ | Set-Content -Path "$Project\src\a.cpp" -NoNewline

@'
int b();
'@ | Set-Content -Path "$Project\src\b.h" -NoNewline

@'
#include "b.h"
int b() { return 7; }
'@ | Set-Content -Path "$Project\src\b.cpp" -NoNewline

@'
#include "a.cpp"
#include "b.cpp"
#include <stdio.h>
int main() { printf("%d %d\n", a(), b()); return 0; }
'@ | Set-Content -Path "$Project\src\main.cpp" -NoNewline

@'
cmake_minimum_required(VERSION 3.20)
project(sninja_sample CXX)
add_library(core STATIC src/a.cpp src/b.cpp)
target_include_directories(core PUBLIC src)
add_executable(app src/main.cpp)
target_link_libraries(app PRIVATE core)
'@ | Set-Content -Path "$Project\CMakeLists.txt" -NoNewline

"created $Project"
