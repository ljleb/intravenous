cmake_minimum_required(VERSION 3.20)

project(test_local_module LANGUAGES CXX)

add_library(iv_runtime_module SHARED
    ${IV_MODULE_ENTRY_FILE}
    ${IV_CORE_INCLUDE_DIR}/basic_nodes/filters.cpp
    ${IV_CORE_INCLUDE_DIR}/basic_nodes/noise.cpp
    ${IV_CORE_INCLUDE_DIR}/basic_nodes/predictors.cpp
    ${IV_CORE_INCLUDE_DIR}/basic_nodes/shaping.cpp
)
target_compile_features(iv_runtime_module PRIVATE cxx_std_23)
set_target_properties(iv_runtime_module PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME ${IV_MODULE_OUTPUT_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${IV_MODULE_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY_DEBUG ${IV_MODULE_OUTPUT_DIR}
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${IV_MODULE_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${IV_MODULE_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY_DEBUG ${IV_MODULE_OUTPUT_DIR}
    LIBRARY_OUTPUT_DIRECTORY_RELEASE ${IV_MODULE_OUTPUT_DIR}
)

target_include_directories(iv_runtime_module PRIVATE
    ${IV_CORE_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(iv_runtime_module SYSTEM PRIVATE
    ${IV_THIRD_PARTY_INCLUDE_DIR}
)

if(DEFINED IV_MODULE_PCH_HEADER)
    target_precompile_headers(iv_runtime_module PRIVATE ${IV_MODULE_PCH_HEADER})
endif()

if(MSVC)
    target_compile_options(iv_runtime_module PRIVATE /W4 /permissive-)
else()
    target_compile_options(iv_runtime_module PRIVATE -Wall -Wextra -Wpedantic)
endif()
