cmake_minimum_required(VERSION 3.20)

project(test_local_module LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${IV_CORE_INCLUDE_DIR}/module/template/SourceSpanRewrite.cmake)

add_library(iv_runtime_module__compile_settings INTERFACE)
target_compile_features(iv_runtime_module__compile_settings INTERFACE cxx_std_23)
target_include_directories(iv_runtime_module__compile_settings INTERFACE
    ${IV_CORE_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(iv_runtime_module__compile_settings SYSTEM INTERFACE
    ${IV_THIRD_PARTY_INCLUDE_DIR}
)

if(MSVC)
    target_compile_options(iv_runtime_module__compile_settings INTERFACE /W4 /permissive-)
else()
    target_compile_options(iv_runtime_module__compile_settings INTERFACE -Wall -Wextra -Wpedantic)
endif()

set(iv_runtime_module_sources ${IV_MODULE_ENTRY_FILE})
iv_rewrite_sources_to_build_dir(
    iv_runtime_module_sources
    TARGET iv_runtime_module
    COMPILE_SETTINGS_TARGET iv_runtime_module__compile_settings
    SOURCES ${iv_runtime_module_sources}
)

add_library(iv_runtime_module SHARED
    ${iv_runtime_module_sources}
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

target_link_libraries(iv_runtime_module PRIVATE iv_runtime_module__compile_settings)

if(DEFINED IV_MODULE_PCH_HEADER)
    target_precompile_headers(iv_runtime_module PRIVATE ${IV_MODULE_PCH_HEADER})
endif()
