cmake_minimum_required(VERSION 3.20)

project(test_local_module LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${IV_SOURCE_DIR}/module/template/ModuleSupport.cmake)

# This file intentionally remains custom CMake. The package helper supplies
# compiler/finalizer settings while this project remains free to add ordinary
# sources, libraries, compile options, JUCE setup, etc.
iv_add_package(iv_package)

if(MSVC)
    target_compile_options(iv_package__compile_settings INTERFACE /W4 /permissive-)
else()
    target_compile_options(iv_package__compile_settings INTERFACE -Wall -Wextra -Wpedantic)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_link_libraries(iv_package PRIVATE stdc++exp)
endif()
