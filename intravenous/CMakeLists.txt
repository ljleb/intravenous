cmake_minimum_required(VERSION 3.20)

project(intravenous LANGUAGES C CXX)

add_library(intravenous_module_runtime STATIC
    module/loader.cpp
    module/watcher.cpp
)

target_compile_features(intravenous_module_runtime PUBLIC cxx_std_23)
set_target_properties(intravenous_module_runtime PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous_module_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(intravenous_module_runtime SYSTEM PUBLIC
    third_party/
)

if(MSVC)
    target_compile_options(intravenous_module_runtime PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous_module_runtime PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(intravenous
    third_party/miniaudio/miniaudio.c
    main.cpp
)

set_source_files_properties(
    third_party/miniaudio/miniaudio.c
    PROPERTIES LANGUAGE C
)

target_compile_features(intravenous PRIVATE cxx_std_23)
set_target_properties(intravenous PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(intravenous PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(intravenous SYSTEM PRIVATE
    third_party/
)

if(MSVC)
    target_compile_options(intravenous PRIVATE /W4 /permissive-)
else()
    target_compile_options(intravenous PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_link_libraries(intravenous PRIVATE intravenous_module_runtime)
