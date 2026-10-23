# ADR-244 slice 5, G-C12 ("recorded as enforced"): every place that emits a sandbox "create" event must say what the
# environment was started under. A `stage == "create"` SandboxExec event without `limits` is a recorded choice nobody
# can compare with the enforced one (ADR-244 §2.1, "Engine floors are named").
#
# The scan finds each `"create"` literal in a file that emits SandboxExec events (it names `SandboxExec` or
# `sandbox_exec_sink`) and requires one of the accepted limit spellings within a window around it (the same statement
# or the neighbouring assignment). tests/fixtures/sandbox_exec_scan holds a violating and a conforming sample: the scan must
# fail on the first and pass on the second (the positive control that proves it can fail).
#
# Run as: cmake -DSCAN_ROOTS="<dir>;<dir>" -P cmake/check_sandbox_exec_limits.cmake
# Script mode sets no policies: without this CMP0012 is OLD and `while(TRUE)` never runs.
cmake_policy(VERSION 3.20)
if(NOT SCAN_ROOTS)
  message(FATAL_ERROR "SCAN_ROOTS is required")
endif()
set(window 700)

set(candidates "")
foreach(root IN LISTS SCAN_ROOTS)
  file(GLOB_RECURSE found "${root}/*.cpp" "${root}/*.hpp" "${root}/*.h")
  list(APPEND candidates ${found})
endforeach()

set(violations "")
set(emitters 0)
foreach(f IN LISTS candidates)
  file(READ "${f}" text)
  string(FIND "${text}" "SandboxExec" has_event)
  string(FIND "${text}" "sandbox_exec_sink" has_sink)
  if(has_event EQUAL -1 AND has_sink EQUAL -1)
    continue()
  endif()
  set(pos 0)
  while(TRUE)
    string(SUBSTRING "${text}" ${pos} -1 rest)
    string(FIND "${rest}" "\"create\"" rel)
    if(rel EQUAL -1)
      break()
    endif()
    math(EXPR at "${pos} + ${rel}")
    # A comment that merely mentions the stage is not an emitter: look at the line.
    string(SUBSTRING "${text}" 0 ${at} before)
    string(FIND "${before}" "\n" last_nl REVERSE)
    math(EXPR line_start "${last_nl} + 1")
    math(EXPR line_len "${at} - ${line_start}")
    string(SUBSTRING "${text}" ${line_start} ${line_len} line_prefix)
    if(NOT line_prefix MATCHES "//")
      math(EXPR emitters "${emitters} + 1")
      math(EXPR w_start "${at} - ${window}")
      if(w_start LESS 0)
        set(w_start 0)
      endif()
      math(EXPR w_len "${window} * 2")
      string(SUBSTRING "${text}" ${w_start} ${w_len} around)
      # The accepted spellings of "this event states its limits": an assignment to the payload's field, the helper that
      # renders a ResourceLimits, or a call that passes a `limits_` member.
      if(NOT around MATCHES "[.]limits[ 	]*=|describe_limits[(]|[ (]limits_[.]|[ (]limits[ 	]*[,)]")
        list(APPEND violations "${f}")
      endif()
    endif()
    math(EXPR pos "${at} + 8")
  endwhile()
endforeach()

if(violations)
  list(REMOVE_DUPLICATES violations)
  list(JOIN violations "\n  " joined)
  message(FATAL_ERROR "a sandbox \"create\" event is emitted without limits (ADR-244 G-C12):\n  ${joined}")
endif()
if(emitters EQUAL 0)
  message(FATAL_ERROR "check_sandbox_exec_limits found no \"create\" emitter at all -- the scan matched nothing")
endif()
message(STATUS "check_sandbox_exec_limits: ok (${emitters} create emitters)")
