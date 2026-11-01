# CMake's Windows AUTO decoding can turn UTF-8 cl.exe diagnostics into mojibake.
# That silently records zero header dependencies and can leave ABI-incompatible
# object files after a header edit. Probe the actual bytes without transcoding.
if(MSVC)
  set(_sninja_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles")
  file(MAKE_DIRECTORY "${_sninja_probe_dir}")
  set(_sninja_probe_header "${_sninja_probe_dir}/sninja-deps-probe.h")
  set(_sninja_probe_source "${_sninja_probe_dir}/sninja-deps-probe.cpp")
  file(WRITE "${_sninja_probe_header}" "// include-prefix probe\n")
  file(WRITE "${_sninja_probe_source}" "#include \"${_sninja_probe_header}\"\nint probe() { return 0; }\n")
  execute_process(COMMAND "${CMAKE_CXX_COMPILER}" /nologo /showIncludes /c
      "${_sninja_probe_source}" "/Fo${_sninja_probe_dir}/sninja-deps-probe.obj"
    RESULT_VARIABLE _sninja_probe_result OUTPUT_VARIABLE _sninja_probe_output
    ERROR_VARIABLE _sninja_probe_error ENCODING NONE)
  if(_sninja_probe_result EQUAL 0)
    string(REPLACE "\\" "/" _sninja_probe_output "${_sninja_probe_output}")
    string(FIND "${_sninja_probe_output}" "${_sninja_probe_header}" _sninja_prefix_end)
    if(_sninja_prefix_end GREATER 0)
      string(SUBSTRING "${_sninja_probe_output}" 0 ${_sninja_prefix_end} _sninja_before_header)
      string(REGEX MATCH "[^\r\n]*$" CMAKE_CL_SHOWINCLUDES_PREFIX "${_sninja_before_header}")
    endif()
  endif()
  if(NOT _sninja_probe_result EQUAL 0 OR NOT _sninja_prefix_end GREATER 0)
    message(FATAL_ERROR "Could not probe MSVC include output: ${_sninja_probe_output} ${_sninja_probe_error}")
  endif()
endif()
