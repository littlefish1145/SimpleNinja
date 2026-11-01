cmake_minimum_required(VERSION 3.20)
project(sninja LANGUAGES CXX)

# sninja - a simplified, SIMD-accelerated reimplementation of ninja.
#
# Layout:
#   src/core   portable core (lexer, graph, state, parser, build engine)
#   src/simd   hand-written x86-64 assembly + runtime dispatch
#   src/cli    the sninja executable
#   tests      support/ (gtest shim + fixtures), core/, build/
#
# Source lists are globbed rather than spelled out: this keeps the build files
# honest as files move between the core and the SIMD layer, and CONFIGURE_DEPENDS
# re-globs whenever a file is added or removed.

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

enable_language(ASM_NASM)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  # C/CXX only: NASM does not know these flags.
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wall>")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wextra>")
endif()

if(MSVC OR (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC"))
  # Ninja deliberately uses the CRT's non-secure C functions, and clang 18
  # predates the MSVC STL's minimum supported clang version.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS
                          _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
endif()

# ---------------------------------------------------------------------------
# sninja_legacy_core: everything except the CLI entry point.
# ---------------------------------------------------------------------------

file(GLOB SNINJA_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/simd/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/simd/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/simd/*.asm"
)
list(SORT SNINJA_CORE_SOURCES)

add_library(sninja_legacy_core STATIC ${SNINJA_CORE_SOURCES})
target_include_directories(sninja_legacy_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core
  ${CMAKE_CURRENT_SOURCE_DIR}/src/simd
)

# ---------------------------------------------------------------------------
# sninja: the command line tool.
# ---------------------------------------------------------------------------

add_executable(sninja_legacy src/cli/main.cpp)
target_link_libraries(sninja_legacy PRIVATE sninja_legacy_core)

# ---------------------------------------------------------------------------
# sninja_legacy_test: the ported upstream test suites.
# ---------------------------------------------------------------------------

file(GLOB SNINJA_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/core/*.cc"
  "${CMAKE_CURRENT_SOURCE_DIR}/tests/build/*.cc"
)
list(SORT SNINJA_TEST_SOURCES)

add_executable(sninja_legacy_test ${SNINJA_TEST_SOURCES})
target_include_directories(sninja_legacy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_link_libraries(sninja_legacy_test PRIVATE sninja_legacy_core)

# The source tree carries a Low mandatory-integrity label, which every freshly
# linked binary inherits.  A Low-integrity process cannot write into
# Medium-integrity directories (e.g. %TEMP%\...), which makes it fail to build
# projects living outside the tree.  Lift the label on our own binaries.
if(WIN32)
  foreach(bin_target sninja_legacy sninja_legacy_test)
    add_custom_command(TARGET ${bin_target} POST_BUILD
      COMMAND icacls $<TARGET_FILE:${bin_target}> /setintegritylevel M
      VERBATIM)
  endforeach()
endif()

enable_testing()
add_test(NAME legacy_scan_test COMMAND sninja_legacy_test --gtest_filter=Scan.*)
add_test(NAME legacy_steal_test COMMAND sninja_legacy_test --gtest_filter=Steal.*)
add_test(NAME legacy_lexer_test COMMAND sninja_legacy_test --gtest_filter=Lexer.*)
add_test(NAME legacy_manifest_parser_test COMMAND sninja_legacy_test --gtest_filter=*Parser*)
