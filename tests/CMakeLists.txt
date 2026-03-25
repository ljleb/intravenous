add_executable(intravenous_architecture_smoke
    architecture_smoke.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_architecture_smoke PRIVATE cxx_std_23)
set_target_properties(intravenous_architecture_smoke PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_architecture_smoke PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_architecture_smoke SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_architecture_smoke PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_architecture_smoke PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous_architecture_smoke PRIVATE intravenous_module_runtime)

add_test(NAME intravenous_architecture_smoke COMMAND intravenous_architecture_smoke)

add_executable(intravenous_module_loader_variants
    module_loader_variants.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_module_loader_variants PRIVATE cxx_std_23)
set_target_properties(intravenous_module_loader_variants PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_module_loader_variants PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_module_loader_variants SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_module_loader_variants PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_module_loader_variants PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous_module_loader_variants PRIVATE intravenous_module_runtime)

add_test(NAME intravenous_module_loader_variants COMMAND intravenous_module_loader_variants)

add_executable(intravenous_module_loader_failures
    module_loader_failures.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_module_loader_failures PRIVATE cxx_std_23)
set_target_properties(intravenous_module_loader_failures PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_module_loader_failures PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_module_loader_failures SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_module_loader_failures PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_module_loader_failures PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous_module_loader_failures PRIVATE intravenous_module_runtime)

add_test(NAME intravenous_module_loader_failures COMMAND intravenous_module_loader_failures)

add_executable(intravenous_module_loader_reload
    module_loader_reload.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_module_loader_reload PRIVATE cxx_std_23)
set_target_properties(intravenous_module_loader_reload PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_module_loader_reload PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_module_loader_reload SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_module_loader_reload PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_module_loader_reload PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous_module_loader_reload PRIVATE intravenous_module_runtime)

add_test(NAME intravenous_module_loader_reload COMMAND intravenous_module_loader_reload)

add_executable(intravenous_module_watcher
    module_watcher.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_module_watcher PRIVATE cxx_std_23)
set_target_properties(intravenous_module_watcher PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_module_watcher PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_module_watcher SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_module_watcher PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_module_watcher PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous_module_watcher PRIVATE intravenous_module_runtime)

add_test(NAME intravenous_module_watcher COMMAND intravenous_module_watcher)

add_executable(intravenous_module_build_behavior
    module_build_behavior.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_module_build_behavior PRIVATE cxx_std_23)
set_target_properties(intravenous_module_build_behavior PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_module_build_behavior PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_module_build_behavior SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_module_build_behavior PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_module_build_behavior PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous_module_build_behavior PRIVATE intravenous_module_runtime)

add_test(NAME intravenous_module_build_behavior COMMAND intravenous_module_build_behavior)

add_executable(intravenous_shared_buffer_context
    shared_buffer_context.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_shared_buffer_context PRIVATE cxx_std_23)
set_target_properties(intravenous_shared_buffer_context PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_shared_buffer_context PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_shared_buffer_context SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_shared_buffer_context PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_shared_buffer_context PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous_shared_buffer_context PRIVATE intravenous_module_runtime)

add_test(NAME intravenous_shared_buffer_context COMMAND intravenous_shared_buffer_context)

add_executable(intravenous_detach_regression
    detach_regression.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_detach_regression PRIVATE cxx_std_23)
set_target_properties(intravenous_detach_regression PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_detach_regression PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_detach_regression SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_detach_regression PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_detach_regression PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous_detach_regression PRIVATE intravenous_module_runtime)

add_test(NAME intravenous_detach_regression COMMAND intravenous_detach_regression)

add_executable(intravenous_block_processing
    block_processing.cpp
    ${PROJECT_SOURCE_DIR}/intravenous/third_party/miniaudio/miniaudio.c
)

target_compile_features(intravenous_block_processing PRIVATE cxx_std_23)
set_target_properties(intravenous_block_processing PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_block_processing PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous
)

target_include_directories(intravenous_block_processing SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/intravenous/third_party
)

if(MSVC)
    target_compile_options(intravenous_block_processing PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_block_processing PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous_block_processing PRIVATE intravenous_module_runtime)

add_test(NAME intravenous_block_processing COMMAND intravenous_block_processing)
