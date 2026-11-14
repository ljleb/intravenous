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
