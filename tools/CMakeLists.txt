cmake_minimum_required(VERSION 3.20)

project(iv_gcc_source_introspection_plugin LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
    message(FATAL_ERROR
        "iv_gcc_source_introspection_plugin requires GCC 16 or newer; "
        "configured compiler is ${CMAKE_CXX_COMPILER_ID} "
        "${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})")
endif()

execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=plugin
    OUTPUT_VARIABLE IV_GCC_PLUGIN_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE IV_GCC_PLUGIN_DIR_RESULT)
if(NOT IV_GCC_PLUGIN_DIR_RESULT EQUAL 0
   OR NOT EXISTS "${IV_GCC_PLUGIN_DIR}/include/gcc-plugin.h")
    message(FATAL_ERROR
        "GCC plugin headers were not found under '${IV_GCC_PLUGIN_DIR}/include'.")
endif()

add_library(iv_gcc_source_introspection_plugin MODULE
    iv_gcc_source_introspection_plugin.cpp)
set_target_properties(iv_gcc_source_introspection_plugin PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    PREFIX "")
target_include_directories(iv_gcc_source_introspection_plugin PRIVATE
    "${IV_GCC_PLUGIN_DIR}/include")
target_compile_options(iv_gcc_source_introspection_plugin PRIVATE
    -fno-rtti
    -Wall
    -Wextra
    -Wpedantic)
