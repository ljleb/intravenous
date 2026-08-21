cmake_minimum_required(VERSION 3.20)

project(test_local_module LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${IV_SOURCE_DIR}/module/template/ModuleSupport.cmake)

# This file intentionally remains custom CMake. IV supplies the generated
# definition imports and root export TU through variables; this project remains
# free to add ordinary sources, libraries, compile options, JUCE setup, etc.
iv_add_runtime_module(iv_runtime_module)

if(MSVC)
    target_compile_options(iv_runtime_module__compile_settings INTERFACE /W4 /permissive-)
else()
    target_compile_options(iv_runtime_module__compile_settings INTERFACE -Wall -Wextra -Wpedantic)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(iv_runtime_module PRIVATE stdc++exp)
endif()

if(DEFINED IV_MODULE_PCH_HEADER AND NOT IV_MODULE_PCH_HEADER STREQUAL "")
    target_precompile_headers(iv_runtime_module PRIVATE ${IV_MODULE_PCH_HEADER})
endif()
