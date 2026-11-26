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
    invalid_dual_execution_callbacks.cpp
    "IV_NODE requires exactly one primary callback")

expect_invalid(
    invalid_duplicate_batch_callbacks.cpp
    "IV_NODE requires exactly one primary callback")

expect_invalid(
    invalid_tock_callback_shape.cpp
    "defines tock_coverage but declares no Tock output port"
    "forward propagation requires Tock execution or a scalar Tick coverage rule"
    "defines propagate_reverse_coverage but requires Tock execution and Random Access inputs")

expect_invalid(
    invalid_tock_state.cpp
    "IV_NODE authors one State type"
    "Tock State must be mutable, non-volatile and default constructible.")

expect_invalid(
    invalid_replayable_node.cpp
    "have no State or authored reverse coverage callbacks")

expect_invalid(
    invalid_replay_random_access.cpp
    "use complete buffers for Region/Coverage inputs")

expect_invalid(
    invalid_replay_coverage_callbacks.cpp
    "defines propagate_reverse_coverage but requires Tock execution and Random Access inputs")

expect_invalid(
    invalid_replay_boolean.cpp
    "intrinsically_replayable was replaced by static constexpr TickReplayMode replay_mode")

expect_invalid(
    invalid_replay_persisted_output.cpp
    "and declare only computed Sequential outputs")

expect_invalid(
    invalid_tick_region_output.cpp
    "Tick primitives declare only Sequential outputs; Tock primitives declare only Region or Coverage outputs")

expect_invalid(
    invalid_tick_coverage_output.cpp
    "Tick primitives declare only Sequential outputs; Tock primitives declare only Region or Coverage outputs")

expect_invalid(
    invalid_tock_sequential_output.cpp
    "Tick primitives declare only Sequential outputs; Tock primitives declare only Region or Coverage outputs")

expect_invalid(
    invalid_tick_windowed_input.cpp
    "IV_NODE Tick Region/Coverage event inputs require complete-buffer access")
