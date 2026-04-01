add_executable(intravenous_render_benchmark
    render_benchmark.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_render_benchmark PRIVATE cxx_std_23)
set_target_properties(intravenous_render_benchmark PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_render_benchmark PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_render_benchmark SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_render_benchmark PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_render_benchmark PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous_render_benchmark PRIVATE intravenous_module_runtime)
