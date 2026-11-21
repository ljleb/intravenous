add_executable(iv_module_build_benchmark
    iv_module_build_benchmark.cpp
)
iv_configure_intravenous_target(iv_module_build_benchmark PRIVATE)
target_link_libraries(iv_module_build_benchmark PRIVATE
    iv_builder
    intravenous_module_loader
)
add_dependencies(iv_module_build_benchmark iv_builder)
add_dependencies(iv_module_build_benchmark intravenous_dsl_pch)
iv_link_stacktrace_support(iv_module_build_benchmark)

# Host-side GraphBuilder/BuilderSession/NodeInstances configuration baseline.
# Package definitions are host function pointers, so this measures no package
# compilation. It links the same host runtime objects as the module tests.
add_executable(iv_graph_configuration_benchmark
    iv_graph_configuration_benchmark.cpp
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party/miniaudio/miniaudio.c
)
target_compile_features(iv_graph_configuration_benchmark PRIVATE cxx_std_23)
set_target_properties(iv_graph_configuration_benchmark PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(iv_graph_configuration_benchmark PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_include_directories(iv_graph_configuration_benchmark SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party
)
iv_add_host_runtime_objects(iv_graph_configuration_benchmark)

# Edit-to-activation latency through the server's project pipeline without a
# frontend, audio device, or RPC server (Front 16 priority target). It reads
# the server build's .intravenous_defaults for the built-in package roots.
add_executable(iv_edit_latency_benchmark
    iv_edit_latency_benchmark.cpp
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party/miniaudio/miniaudio.c
)
target_compile_features(iv_edit_latency_benchmark PRIVATE cxx_std_23)
set_target_properties(iv_edit_latency_benchmark PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_include_directories(iv_edit_latency_benchmark PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_include_directories(iv_edit_latency_benchmark SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party
)
target_compile_definitions(iv_edit_latency_benchmark PRIVATE
    IV_EDIT_BENCHMARK_INSTALL_DIR="${PROJECT_BINARY_DIR}/src/intravenous"
)
iv_add_host_runtime_objects(iv_edit_latency_benchmark)
add_dependencies(iv_edit_latency_benchmark intravenous_builtin_package_artifacts)

foreach(benchmark_target IN ITEMS iv_module_build_benchmark iv_edit_latency_benchmark)
    add_dependencies(${benchmark_target} iv-dev-cache)
    target_compile_definitions(${benchmark_target} PRIVATE
        IV_BENCHMARK_DEV_CACHE="$<TARGET_FILE:iv-dev-cache>")
endforeach()

# Same executor and no-DSP graph for both cases; adds the former all-binding
# scan only in the legacy case to isolate idle manual-control polling cost.
add_executable(iv_manual_control_polling_benchmark
    iv_manual_control_polling_benchmark.cpp
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party/miniaudio/miniaudio.c)
target_compile_features(iv_manual_control_polling_benchmark PRIVATE cxx_std_23)
set_target_properties(iv_manual_control_polling_benchmark PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(iv_manual_control_polling_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(iv_manual_control_polling_benchmark SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party)
iv_add_host_runtime_objects(iv_manual_control_polling_benchmark)

# Shared buffered-store allocation, page/view reuse and fragmentation. No DSP
# compilation is performed by the benchmark itself.
add_executable(iv_buffered_storage_benchmark
    iv_buffered_storage_benchmark.cpp
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party/miniaudio/miniaudio.c)
target_compile_features(iv_buffered_storage_benchmark PRIVATE cxx_std_23)
target_include_directories(iv_buffered_storage_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(iv_buffered_storage_benchmark SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party)
iv_add_host_runtime_objects(iv_buffered_storage_benchmark)

# Replay fixed playback projects from build-tree fixture copies.
set(playback_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture")
foreach(playback_project IN ITEMS iv_project independent stacked shared-driver)
    if(playback_project STREQUAL "iv_project")
        set(playback_project_dir "${playback_fixture_dir}")
    else()
        set(playback_project_dir "${CMAKE_CURRENT_BINARY_DIR}/playback_${playback_project}_fixture")
    endif()
    configure_file("fixtures/playback/${playback_project}.jsonl" "${playback_project_dir}/iv_project.jsonl" COPYONLY)
    foreach(playback_package_file IN ITEMS saw/iv_package.json saw/module.cpp)
        configure_file("fixtures/playback/${playback_package_file}" "${playback_project_dir}/${playback_package_file}" COPYONLY)
    endforeach()
endforeach()

add_executable(iv_playback_benchmark
    iv_playback_benchmark.cpp
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party/miniaudio/miniaudio.c)
target_compile_features(iv_playback_benchmark PRIVATE cxx_std_23)
set_target_properties(iv_playback_benchmark PROPERTIES
    CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(iv_playback_benchmark PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_include_directories(iv_playback_benchmark SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/src/intravenous/third_party)
target_compile_definitions(iv_playback_benchmark PRIVATE
    IV_PLAYBACK_BENCHMARK_INSTALL_DIR="${PROJECT_BINARY_DIR}/src/intravenous"
    IV_PLAYBACK_BENCHMARK_WORKSPACE="${playback_fixture_dir}")
iv_add_host_runtime_objects(iv_playback_benchmark)
add_dependencies(iv_playback_benchmark intravenous_builtin_package_artifacts)
