include(FetchContent)
include(GoogleTest)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

function(add_iv_gtest target source)
    set(options ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
    set(oneValueArgs)
    set(multiValueArgs IV_MODULE_COMPILATION_TESTS)
    cmake_parse_arguments(IVGT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${target}
        ${source}
        ${PROJECT_SOURCE_DIR}/src/intravenous/third_party/miniaudio/miniaudio.c
    )

    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_include_directories(${target} PRIVATE
        ${PROJECT_SOURCE_DIR}/src
    )

    target_include_directories(${target} SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/src/intravenous/third_party
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    target_link_libraries(${target} PRIVATE
        GTest::gtest_main
    )
    iv_add_host_runtime_objects(${target})

    gtest_add_tests(TARGET ${target} TEST_LIST discovered_tests)

    foreach(test_name IN LISTS discovered_tests)
        set(test_labels "light")
        if(IVGT_ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
            set(test_labels "heavy")
        elseif(test_name IN_LIST IVGT_IV_MODULE_COMPILATION_TESTS)
            set(test_labels "heavy")
        endif()
        set_tests_properties(${test_name} PROPERTIES LABELS "${test_labels}")
    endforeach()

    set_property(GLOBAL APPEND PROPERTY IV_GTEST_TARGETS ${target})
endfunction()

add_iv_gtest(
    intravenous_architecture_smoke_tests
    architecture_smoke_gtest.cpp
    IV_MODULE_COMPILATION_TESTS
        ArchitectureSmoke.DirectoryModuleRetainsModuleRefsAndRuns
        ArchitectureSmoke.DirectoryModuleEventArrayBindingsResolve)
add_iv_gtest(
    intravenous_module_loader_variants_tests
    module_loader_variants_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(
    intravenous_module_loader_failures_tests
    module_loader_failures_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(
    intravenous_module_loader_reload_tests
    module_loader_reload_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(
    intravenous_module_build_behavior_tests
    module_build_behavior_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(
    intravenous_module_watcher_tests
    module_watcher_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(intravenous_shared_buffer_context_tests shared_buffer_context_gtest.cpp)
add_iv_gtest(intravenous_detach_regression_tests detach_regression_gtest.cpp)
add_iv_gtest(intravenous_block_node_executor_tests node_block_executor_gtest.cpp)
add_iv_gtest(intravenous_timeline_lanes_tests modules/timeline_lanes_gtest.cpp)
add_iv_gtest(intravenous_timeline_execution_tests modules/timeline_execution_gtest.cpp)
add_iv_gtest(intravenous_lane_filters_tests modules/lane_filters_gtest.cpp)
add_iv_gtest(intravenous_lane_views_tests modules/lane_views_gtest.cpp)
add_iv_gtest(intravenous_lanes_visualization_tests modules/lanes_visualization_gtest.cpp)
add_iv_gtest(intravenous_lane_query_tests query/lane_query_gtest.cpp)
add_iv_gtest(intravenous_local_gamma_time_aligner_tests local_gamma_time_aligner_gtest.cpp)
add_iv_gtest(intravenous_startup_config_tests modules/startup_config_gtest.cpp)
add_iv_gtest(intravenous_iv_module_definitions_tests modules/iv_module_definitions_gtest.cpp)
add_iv_gtest(intravenous_iv_module_instances_tests modules/iv_module_instances_gtest.cpp)
add_iv_gtest(
    intravenous_iv_module_instances_execution_tests
    modules/iv_module_instances_execution_gtest.cpp)
add_iv_gtest(
    intravenous_iv_module_reload_tests
    modules/iv_module_reload_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(
    intravenous_iv_module_source_introspection_tests
    modules/iv_module_source_introspection_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(intravenous_graph_input_lanes_tests modules/graph_input_lanes_gtest.cpp)
add_iv_gtest(intravenous_graph_logical_outputs_tests graph_logical_outputs_gtest.cpp)
add_iv_gtest(intravenous_task_runner_tests modules/task_runner_gtest.cpp)
add_iv_gtest(intravenous_lane_filters_bridges_tests bridges/lane_filters_bridges_gtest.cpp)
add_iv_gtest(intravenous_runtime_module_bridges_tests bridges/runtime_module_bridges_gtest.cpp)
add_iv_gtest(
    intravenous_runtime_integration_tests
    integration_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(intravenous_socket_rpc_server_tests socket_rpc_server_gtest.cpp)
add_iv_gtest(intravenous_socket_rpc_support_tests socket_rpc_support_gtest.cpp)
add_iv_gtest(
    intravenous_socket_rpc_iv_module_source_introspection_bridge_tests
    bridges/socket_rpc_iv_module_source_introspection_bridge_gtest.cpp
    IV_MODULE_COMPILATION_TESTS
        SocketRpcIvModuleSourceIntrospectionBridge.BoundQueryEventPopulatesBuilderFromOwners)
add_iv_gtest(
    intravenous_socket_rpc_iv_module_instances_bridge_tests
    bridges/socket_rpc_iv_module_instances_bridge_gtest.cpp)
add_iv_gtest(
    intravenous_socket_rpc_timeline_execution_bridge_tests
    bridges/socket_rpc_timeline_execution_bridge_gtest.cpp)
add_iv_gtest(
    intravenous_socket_rpc_notification_bridge_tests
    bridges/socket_rpc_notification_bridge_gtest.cpp)
add_executable(intravenous_node_layout_storage
    node_layout_storage.cpp
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_node_layout_storage PRIVATE cxx_std_23)
set_target_properties(intravenous_node_layout_storage PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_node_layout_storage PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)

target_include_directories(intravenous_node_layout_storage SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_node_layout_storage PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_node_layout_storage PRIVATE -Wall -Wextra -Wpedantic)
endif()

iv_add_host_runtime_objects(intravenous_node_layout_storage)

add_test(NAME intravenous_node_layout_storage COMMAND intravenous_node_layout_storage)
set_tests_properties(intravenous_node_layout_storage PROPERTIES LABELS "light")

get_property(iv_gtest_targets GLOBAL PROPERTY IV_GTEST_TARGETS)
add_custom_target(intravenous_light_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure -L light
    DEPENDS ${iv_gtest_targets} intravenous_node_layout_storage
    USES_TERMINAL
)
