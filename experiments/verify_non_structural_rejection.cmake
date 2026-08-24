if(NOT DEFINED CXX OR NOT DEFINED SOURCE OR NOT DEFINED OBJECT)
    message(FATAL_ERROR "CXX, SOURCE, and OBJECT are required")
endif()

execute_process(
    COMMAND
        "${CXX}"
        -std=c++26
        -freflection
        -c
        "${SOURCE}"
        -o
        "${OBJECT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)

if(result EQUAL 0)
    message(FATAL_ERROR "GCC unexpectedly reflected a non-structural value")
endif()

set(diagnostics "${output}\n${error}")
if(NOT diagnostics MATCHES "must be a cv-unqualified structural type")
    message(FATAL_ERROR "GCC failed for an unexpected reason:\n${diagnostics}")
endif()
