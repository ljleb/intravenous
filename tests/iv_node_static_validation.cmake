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
    "declares a Tock output port; define tock_coverage"
    "define exact propagate_forward_coverage")

expect_invalid(
    invalid_internal_node.cpp
    "concrete node ports must be declared by static constexpr inputs() and outputs()")

expect_invalid(
    invalid_tick_access_to_tock_output.cpp
    "tick_block() cannot write a Tock sample output"
    "tick_block() cannot write a Tock event output")

expect_invalid(
    invalid_tock_callback_shape.cpp
    "defines tock_coverage but declares no Tock output port"
    "defines propagate_forward_coverage but declares no Tock output port"
    "defines propagate_reverse_coverage but does not declare both random-access input and Tock output ports")

expect_invalid(
    invalid_tock_state.cpp
    "Node::TockState must be a mutable, non-volatile, default-constructible object type")

expect_invalid(
    invalid_replayable_node.cpp
    "IV_NODE intrinsically replayable nodes must author tick() (not tick_block())")
