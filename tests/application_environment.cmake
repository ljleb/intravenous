if(NOT DEFINED IV_SOURCE_DIR OR NOT DEFINED IV_BINARY_DIR)
    message(FATAL_ERROR "application environment test needs source and binary directories")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef suffix)
set(workspace "${IV_BINARY_DIR}/environment-test-${suffix}")
file(MAKE_DIRECTORY "${workspace}")
set(capture "${workspace}/capture.cmake")
set(launcher "${workspace}/iv-toolchain-environment")
file(WRITE "${capture}" "set(CMAKE_CURRENT_BINARY_DIR [==[${workspace}]==])\nset(CMAKE_C_COMPILER /fixture/clang)\nset(CMAKE_CXX_COMPILER /fixture/clang++)\nset(CMAKE_CXX_COMPILER_VERSION 23.1.0)\nset(ENV{NIX_HARDENING_ENABLE_iv_environment_test} \"\${IV_CAPTURE_VALUE}\")\nunset(ENV{NIX_CFLAGS_COMPILE_iv_environment_foreign})\nset(ENV{IV_ENVIRONMENT_SECRET} do-not-save-this)\ninclude([==[${IV_SOURCE_DIR}/src/intravenous/module/template/ApplicationEnvironment.cmake]==])\n")
set(assertion "${workspace}/assert.cmake")
file(WRITE "${assertion}" [=[
if(NOT "$ENV{NIX_HARDENING_ENABLE_iv_environment_test}" STREQUAL "${IV_EXPECTED}")
    message(FATAL_ERROR "captured compiler environment was not restored")
endif()
if(DEFINED ENV{NIX_CFLAGS_COMPILE_iv_environment_foreign})
    message(FATAL_ERROR "a foreign compiler flag survived restoration")
endif()
if(NOT "$ENV{IV_RUNTIME_AUDIO_TEST}" STREQUAL "keep-runtime-state")
    message(FATAL_ERROR "runtime environment was overwritten")
endif()
if(NOT "$ENV{IV_ENVIRONMENT_SECRET}" STREQUAL "runtime-secret")
    message(FATAL_ERROR "unrelated user environment was overwritten")
endif()
]=])
function(capture_environment value refresh)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DIV_CAPTURE_VALUE=${value}"
        "-DIV_REFRESH_APPLICATION_ENVIRONMENT=${refresh}" -P "${capture}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "environment capture failed: ${output}${error}")
    endif()
endfunction()
function(check_environment expected)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env
        NIX_HARDENING_ENABLE_iv_environment_test=wrong
        NIX_CFLAGS_COMPILE_iv_environment_foreign=must-be-unset
        IV_RUNTIME_AUDIO_TEST=keep-runtime-state IV_ENVIRONMENT_SECRET=runtime-secret
        "${launcher}" "${CMAKE_COMMAND}" "-DIV_EXPECTED=${expected}" -P "${assertion}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "environment activation failed: ${output}${error}")
    endif()
endfunction()

# Include shell metacharacters to test literal serialization, not evaluation.
set(original [=[flags ' " $HOME $(false); @CMAKE_COMMAND@ literals]=])
capture_environment("${original}" OFF)
check_environment("${original}")
file(READ "${launcher}" first)
if(first MATCHES "do-not-save-this|IV_ENVIRONMENT_SECRET")
    message(FATAL_ERROR "capture persisted unrelated user environment")
endif()

# Automatic regeneration must retain the original environment, even when its
# parent is a desktop process with a different environment.
capture_environment(changed OFF)
check_environment("${original}")
file(READ "${launcher}" second)
if(NOT first STREQUAL second)
    message(FATAL_ERROR "automatic regeneration changed the saved environment")
endif()
capture_environment(changed ON)
check_environment(changed)

# The launcher must preserve the inherited descriptor used for the server RPC.
execute_process(COMMAND /bin/sh -c
    [=[exec 3>"$1"; exec "$2" /bin/sh -c 'printf preserved >&3']=]
    iv-environment-test "${workspace}/fd3" "${launcher}"
    RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "environment launcher lost the inherited RPC descriptor")
endif()
file(READ "${workspace}/fd3" descriptor_output)
if(NOT descriptor_output STREQUAL preserved)
    message(FATAL_ERROR "environment launcher did not preserve fd 3")
endif()
file(REMOVE_RECURSE "${workspace}")
