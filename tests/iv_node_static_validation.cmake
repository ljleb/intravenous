if(NOT DEFINED IV_CXX_COMPILER OR NOT DEFINED IV_SOURCE_DIR)
    message(FATAL_ERROR "IV_NODE validation test requires IV_CXX_COMPILER and IV_SOURCE_DIR")
endif()

set(_iv_node_validation_dir "${IV_SOURCE_DIR}/tests/iv_node_static_validation")

function(expect_valid source)
    execute_process(
        COMMAND "${IV_CXX_COMPILER}" -std=c++23 "-I${IV_SOURCE_DIR}/src"
            -fsyntax-only -ferror-limit=0 "${_iv_node_validation_dir}/${source}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "expected IV_NODE validation source '${source}' to compile:\n${stdout}${stderr}")
    endif()
endfunction()

function(expect_invalid source)
    execute_process(
        COMMAND "${IV_CXX_COMPILER}" -std=c++23 "-I${IV_SOURCE_DIR}/src"
            -fsyntax-only -ferror-limit=0 "${_iv_node_validation_dir}/${source}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    if(result EQUAL 0)
        message(FATAL_ERROR
            "expected IV_NODE validation source '${source}' to fail compilation")
    endif()
    set(output "${stdout}${stderr}")
    foreach(diagnostic IN LISTS ARGN)
        string(FIND "${output}" "${diagnostic}" diagnostic_offset)
        if(diagnostic_offset EQUAL -1)
            message(FATAL_ERROR
                "IV_NODE validation source '${source}' did not report '${diagnostic}':\n${output}")
        endif()
    endforeach()
endfunction()

expect_valid(valid.cpp)
expect_invalid(
    invalid.cpp
    "IV_NODE requires inputs() and outputs()"
    "declares a compiled sample port"
    "must define only one compiled-access callback")
