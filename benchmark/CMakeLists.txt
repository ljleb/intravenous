add_executable(iv_module_build_benchmark
    iv_module_build_benchmark.cpp
)
iv_configure_intravenous_target(iv_module_build_benchmark PRIVATE)
target_link_libraries(iv_module_build_benchmark PRIVATE
    intravenous_graph_builder
    intravenous_module_loader
)
add_dependencies(iv_module_build_benchmark iv_module_shared)
iv_link_stacktrace_support(iv_module_build_benchmark)

add_executable(iv_module_execution_benchmark
    iv_module_execution_benchmark.cpp
)
iv_configure_intravenous_target(iv_module_execution_benchmark PRIVATE)
target_link_libraries(iv_module_execution_benchmark PRIVATE
    intravenous_graph_builder
    intravenous_module_loader
    ${CMAKE_DL_LIBS}
)
iv_link_stacktrace_support(iv_module_execution_benchmark)
