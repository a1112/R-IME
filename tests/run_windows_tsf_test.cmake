# A native ACP document must receive foreground focus before TSF activates its
# keyboard sink. The SDK debugger supplies Windows' documented foreground
# exception. Only the test host runs under it, never the input method in use.
foreach(required TEST_HOST TIP_DLL DEBUGGER OUTPUT_DIRECTORY)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "Missing ${required}")
  endif()
endforeach()
file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(result "${OUTPUT_DIRECTORY}/tsf-${run_id}.json")
set(log "${OUTPUT_DIRECTORY}/tsf-${run_id}.log")
set(stderr "${OUTPUT_DIRECTORY}/tsf-${run_id}.stderr.log")
execute_process(
  COMMAND "${DEBUGGER}" -g -G -c "sxd e06d7363; g" "${TEST_HOST}" "${TIP_DLL}" "${result}"
  OUTPUT_FILE "${log}" ERROR_FILE "${stderr}" TIMEOUT 90 RESULT_VARIABLE debugger_result)
if(NOT EXISTS "${result}")
  message(FATAL_ERROR "TSF host produced no result (${debugger_result}); inspect ${log} and ${stderr}")
endif()
file(READ "${result}" result_json)
string(JSON success ERROR_VARIABLE parse_error GET "${result_json}" success)
if(parse_error OR NOT success)
  string(JSON detail ERROR_VARIABLE message_error GET "${result_json}" message)
  message(FATAL_ERROR "TSF host failed: ${detail}; inspect ${log} and ${stderr}")
endif()
message(STATUS "Native TSF document regression passed; result: ${result}")
