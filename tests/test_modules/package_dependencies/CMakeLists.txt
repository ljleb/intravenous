cmake_minimum_required(VERSION 3.20)

project(test_package_dependencies LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${IV_SOURCE_DIR}/module/template/ModuleSupport.cmake)

# Deliberately create the dependencies after iv_add_package(). The finalizer
# setup must be deferred until this custom CMake directory has finished.
iv_add_package(iv_package)
get_property(iv_package_type TARGET iv_package PROPERTY TYPE)
if(NOT iv_package_type STREQUAL "OBJECT_LIBRARY")
    message(FATAL_ERROR "iv_package must remain the public OBJECT target")
endif()
if(NOT TARGET iv_package__finalized)
    message(FATAL_ERROR "iv_package__finalized must exist immediately")
endif()
add_custom_target(iv_test_after_finalizer ALL DEPENDS iv_package__finalized)

add_library(iv_test_lto_leaf STATIC lto_leaf.cpp)
add_library(iv_test_lto_root STATIC lto_root.cpp)
foreach(target iv_test_lto_leaf iv_test_lto_root)
    target_compile_features(${target} PRIVATE cxx_std_26)
    target_compile_options(${target} PRIVATE -O0 -flto=full)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()
target_link_libraries(iv_test_lto_root PRIVATE iv_test_lto_leaf)

add_library(iv_test_dynamic SHARED dynamic.cpp)
target_compile_features(iv_test_dynamic PRIVATE cxx_std_26)
target_include_directories(iv_test_dynamic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(iv_test_dynamic PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

target_include_directories(iv_package PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(iv_package PRIVATE iv_test_lto_root iv_test_dynamic)

# Exercise both explicit APIs after ordinary target_link_libraries(). Duplicate
# declaration of the same target must produce one dependency in each final form.
iv_package_add_llvm_inputs(iv_package iv_test_lto_root)
iv_package_add_dynamic_libraries(iv_package iv_test_dynamic)

option(IV_TEST_INCLUDE_NATIVE_ARCHIVE
    "Add a deliberately non-LTO archive, which the finalizer must reject" OFF)
if(IV_TEST_INCLUDE_NATIVE_ARCHIVE)
    add_library(iv_test_native_archive STATIC native_archive.cpp)
    target_compile_features(iv_test_native_archive PRIVATE cxx_std_26)
    target_compile_options(iv_test_native_archive PRIVATE -O0 -fno-lto)
    target_include_directories(iv_test_native_archive PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(iv_package PRIVATE iv_test_native_archive)
endif()
