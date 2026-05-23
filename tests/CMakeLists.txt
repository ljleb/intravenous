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
    add_executable(${target}
        ${source}
        ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
    )

    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    target_include_directories(${target} PRIVATE
        ${PROJECT_SOURCE_DIR}/intravenous
    )

    target_include_directories(${target} SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/intravenous/third_party
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    target_link_libraries(${target} PRIVATE
        GTest::gtest_main
        intravenous_module_runtime
        intravenous_json_rpc_server
        intravenous_tooling_runtime
        nlohmann_json::nlohmann_json
    )

    gtest_discover_tests(${target})
endfunction()

add_iv_gtest(intravenous_architecture_smoke_tests architecture_smoke_gtest.cpp)
add_iv_gtest(intravenous_module_loader_variants_tests module_loader_variants_gtest.cpp)
add_iv_gtest(intravenous_module_loader_failures_tests module_loader_failures_gtest.cpp)
add_iv_gtest(intravenous_module_loader_reload_tests module_loader_reload_gtest.cpp)
add_iv_gtest(intravenous_module_build_behavior_tests module_build_behavior_gtest.cpp)
add_iv_gtest(intravenous_module_watcher_tests module_watcher_gtest.cpp)
add_iv_gtest(intravenous_shared_buffer_context_tests shared_buffer_context_gtest.cpp)
add_iv_gtest(intravenous_detach_regression_tests detach_regression_gtest.cpp)
add_iv_gtest(intravenous_timeline_lanes_tests timeline_lanes_gtest.cpp)
add_iv_gtest(intravenous_lane_views_tests lane_views_gtest.cpp)
add_iv_gtest(intravenous_local_gamma_time_aligner_tests local_gamma_time_aligner_gtest.cpp)
add_iv_gtest(intravenous_iv_module_definitions_tests iv_module_definitions_gtest.cpp)
add_iv_gtest(intravenous_project_introspection_tests project_introspection_gtest.cpp)
add_iv_gtest(intravenous_graph_input_lanes_tests graph_input_lanes_gtest.cpp)
add_iv_gtest(intravenous_runtime_module_bridges_tests runtime_module_bridges_gtest.cpp)
add_iv_gtest(intravenous_runtime_integration_tests runtime_integration_gtest.cpp)
add_iv_gtest(intravenous_socket_rpc_server_tests socket_rpc_server_gtest.cpp)
add_iv_gtest(intravenous_socket_rpc_support_tests socket_rpc_support_gtest.cpp)
add_iv_gtest(
    intravenous_socket_rpc_project_introspection_bridge_tests
    socket_rpc_project_introspection_bridge_gtest.cpp)
add_iv_gtest(
    intravenous_socket_rpc_notification_bridge_tests
    socket_rpc_notification_bridge_gtest.cpp)
add_executable(intravenous_node_layout_storage
    node_layout_storage.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_node_layout_storage PRIVATE cxx_std_23)
set_target_properties(intravenous_node_layout_storage PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_node_layout_storage PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_node_layout_storage SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_node_layout_storage PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_node_layout_storage PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous_node_layout_storage PRIVATE
    intravenous_module_runtime
    intravenous_tooling_runtime
)

add_test(NAME intravenous_node_layout_storage COMMAND intravenous_node_layout_storage)
