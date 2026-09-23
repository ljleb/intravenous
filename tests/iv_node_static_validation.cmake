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
    "declares an indexed output port; define tock_coverage"
    "define exact propagate_forward_coverage")

expect_invalid(
    invalid_indexed_tick_output.cpp
    "tick_block() cannot write an indexed sample output"
    "tick_block() cannot write an indexed event output")

expect_invalid(
    invalid_indexed_callback_shape.cpp
    "defines tock_coverage but declares no indexed output port"
    "defines propagate_forward_coverage but declares no indexed output port"
    "defines propagate_reverse_coverage but does not declare both indexed input and indexed output ports")

expect_invalid(
    invalid_indexed_state.cpp
    "Node::IndexedState must be a mutable, non-volatile, default-constructible object type")
