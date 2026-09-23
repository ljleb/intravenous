include(FetchContent)
include(GoogleTest)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(gtest PRIVATE -Wno-character-conversion)
endif()

function(add_iv_gtest target source)
    set(options ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION NO_HOST_RUNTIME_OBJECTS)
    set(oneValueArgs)
    set(multiValueArgs IV_MODULE_COMPILATION_TESTS)
    cmake_parse_arguments(IVGT "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${target} ${source})

    if(NOT IVGT_NO_HOST_RUNTIME_OBJECTS)
        target_sources(${target} PRIVATE
            ${PROJECT_SOURCE_DIR}/src/intravenous/third_party/miniaudio/miniaudio.c
        )
    endif()

    target_compile_features(${target} PRIVATE cxx_std_23)
    target_compile_definitions(${target} PRIVATE
        IV_CONFIGURED_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_include_directories(${target} PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_include_directories(${target} SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/src/intravenous/third_party
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -Wno-c2y-extensions)
        endif()
    endif()

    target_link_libraries(${target} PRIVATE
        GTest::gtest_main
    )
    if(NOT IVGT_NO_HOST_RUNTIME_OBJECTS)
        iv_add_host_runtime_objects(${target})
    endif()

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
    intravenous_module_loader_failures_tests
    module_loader_failures_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(
    intravenous_module_package_dependencies_tests
    module_package_dependencies_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(
    intravenous_configured_graph_binary_archive_tests
    configured_graph_binary_archive_gtest.cpp)
add_iv_gtest(
    intravenous_configured_graph_embedding_tests
    configured_graph_embedding_gtest.cpp)
add_iv_gtest(
    intravenous_node_build_request_tests
    node_build_request_gtest.cpp)

add_test(
    NAME intravenous_iv_node_static_validation
    COMMAND ${CMAKE_COMMAND}
        "-DIV_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DIV_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/iv_node_static_validation.cmake")
set_tests_properties(intravenous_iv_node_static_validation PROPERTIES LABELS "light")

add_iv_gtest(intravenous_module_test_utils_tests module_test_utils_gtest.cpp)
add_iv_gtest(
    intravenous_module_watcher_tests
    module_watcher_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(
    intravenous_module_context_boundary_tests
    module_context_boundary_gtest.cpp)
add_iv_gtest(intravenous_compat_tests compat_gtest.cpp)
add_iv_gtest(
    intravenous_linker_event_propagation_tests
    linker_event_propagation_gtest.cpp
    NO_HOST_RUNTIME_OBJECTS)
add_iv_gtest(intravenous_shared_buffer_context_tests shared_buffer_context_gtest.cpp)
add_iv_gtest(intravenous_channels_tests channels_gtest.cpp)
add_iv_gtest(intravenous_graph_modules_tests graph_modules_gtest.cpp)
add_iv_gtest(intravenous_predictors_tests predictors_gtest.cpp)
add_iv_gtest(intravenous_lane_filters_tests modules/lane_filters_gtest.cpp)
add_iv_gtest(intravenous_lane_views_tests modules/lane_views_gtest.cpp)
add_iv_gtest(intravenous_lanes_visualization_tests modules/lanes_visualization_gtest.cpp)
add_iv_gtest(intravenous_system_audio_devices_tests modules/system_audio_devices_gtest.cpp)
add_iv_gtest(intravenous_lane_query_tests query/lane_query_gtest.cpp)
add_iv_gtest(intravenous_startup_config_tests modules/startup_config_gtest.cpp)
add_iv_gtest(intravenous_node_definitions_tests modules/node_definitions_gtest.cpp)
add_iv_gtest(intravenous_node_instances_tests modules/node_instances_gtest.cpp)
add_iv_gtest(intravenous_project_graph_tests modules/project_graph_gtest.cpp)
add_iv_gtest(intravenous_graph_connections_tests modules/graph_connections_gtest.cpp)
add_iv_gtest(
    intravenous_graph_jit_connection_plan_tests
    graph_jit_connection_plan_gtest.cpp)
add_iv_gtest(
    intravenous_graph_jit_tests
    modules/graph_jit_gtest.cpp
    IV_MODULE_COMPILATION_TESTS
        GraphJitSharedRuntimeFixture.BuildPackage)

# The runtime GraphJIT regression package is expensive to compile, while the
# individual graph compilation/execution scenarios are independent. Build the
# shared package once, then allow CTest to schedule all scenario tests in
# parallel against the stable on-disk package artifact.
get_property(iv_graph_jit_ctests DIRECTORY PROPERTY TESTS)
foreach(test_name IN LISTS iv_graph_jit_ctests)
    if(test_name STREQUAL "GraphJitSharedRuntimeFixture.BuildPackage")
        set_tests_properties(${test_name} PROPERTIES
            FIXTURES_SETUP graph_jit_runtime_package
            LABELS "heavy")
    elseif(test_name MATCHES "^GraphJitRuntimeFixture\\.")
        set_tests_properties(${test_name} PROPERTIES
            FIXTURES_REQUIRED graph_jit_runtime_package
            LABELS "heavy")
    endif()
endforeach()
add_iv_gtest(
    intravenous_node_instances_configuration_tests
    modules/node_instances_configuration_gtest.cpp)
add_iv_gtest(intravenous_package_watcher_tests modules/package_watcher_gtest.cpp)
add_iv_gtest(intravenous_package_definitions_tests modules/package_definitions_gtest.cpp)
add_iv_gtest(intravenous_package_pipeline_logic_tests modules/package_pipeline_logic_gtest.cpp)
add_iv_gtest(
    intravenous_package_jit_tests
    modules/package_jit_gtest.cpp
    IV_MODULE_COMPILATION_TESTS
        PackageJit.MixedBatchIsolatesFailuresAndSuccessfulRevisionsAdvanceMonotonically)
add_iv_gtest(
    intravenous_package_pipeline_tests
    modules/package_pipeline_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(
    intravenous_iv_module_source_introspection_tests
    modules/iv_module_source_introspection_gtest.cpp
    ALL_TESTS_REQUIRE_IV_MODULE_COMPILATION)
add_iv_gtest(intravenous_project_persistence_tests project_persistence_gtest.cpp)
add_iv_gtest(
    intravenous_project_autosave_tests
    modules/project_autosave_gtest.cpp)
add_iv_gtest(intravenous_lane_filters_bridges_tests bridges/lane_filters_bridges_gtest.cpp)
add_iv_gtest(intravenous_runtime_module_bridges_tests bridges/runtime_module_bridges_gtest.cpp)
add_iv_gtest(intravenous_socket_rpc_server_tests socket_rpc_server_gtest.cpp)
add_iv_gtest(intravenous_socket_rpc_support_tests socket_rpc_support_gtest.cpp)
add_iv_gtest(
    intravenous_socket_rpc_iv_module_source_introspection_bridge_tests
    bridges/socket_rpc_iv_module_source_introspection_bridge_gtest.cpp
    IV_MODULE_COMPILATION_TESTS
        SocketRpcIvModuleSourceIntrospectionBridge.BoundQueryEventPopulatesBuilderFromOwners)
add_iv_gtest(
    intravenous_socket_rpc_node_instances_bridge_tests
    bridges/socket_rpc_node_instances_bridge_gtest.cpp)
add_iv_gtest(
    intravenous_socket_rpc_notification_bridge_tests
    bridges/socket_rpc_notification_bridge_gtest.cpp)
add_executable(intravenous_node_layout_storage
    node_layout_storage.cpp
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_node_layout_storage PRIVATE cxx_std_23)
target_compile_definitions(intravenous_node_layout_storage PRIVATE
    IV_CONFIGURED_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
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
