cmake_minimum_required(VERSION 3.21)

project(intravenous-dsp LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 23)
    message(FATAL_ERROR
        "Intravenous modules require Clang 23 or newer for the LLVM reload pipeline; "
        "configured compiler is ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
        "(${CMAKE_CXX_COMPILER})")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)
if(NOT LLVM_VERSION_MAJOR EQUAL 23)
    message(FATAL_ERROR "Intravenous requires LLVM 23; found LLVM ${LLVM_PACKAGE_VERSION}")
endif()

set(JUCE_DIR "" CACHE PATH "Path to a JUCE source tree or installed JUCE package root")

# avoid pulling heavy GUI/web dependencies
# unless explicitly re-enabled by the user at configure time.
set(JUCE_WEB_BROWSER 0 CACHE BOOL "Enable JUCE web browser component" FORCE)
set(JUCE_USE_CURL 0 CACHE BOOL "Enable JUCE curl integration" FORCE)

set(IV_JUCE_SOURCE_DIR "")
if(JUCE_DIR)
    if(EXISTS "${JUCE_DIR}/CMakeLists.txt")
        set(IV_JUCE_SOURCE_DIR "${JUCE_DIR}")
    elseif(EXISTS "${JUCE_DIR}/JUCEConfig.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${JUCE_DIR}")
    elseif(EXISTS "${JUCE_DIR}/lib/cmake/JUCE/JUCEConfig.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${JUCE_DIR}/lib/cmake/JUCE")
    endif()
endif()

if(IV_JUCE_SOURCE_DIR)
    add_subdirectory("${IV_JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/juce" EXCLUDE_FROM_ALL)
    set(JUCE_FOUND TRUE)
else()
    find_package(JUCE CONFIG QUIET)
endif()

add_subdirectory(src/intravenous)
add_subdirectory(tools)

add_dependencies(
    intravenous_module_loader
    iv_clang_source_introspection_plugin
    iv_module_finalize)
target_compile_definitions(intravenous_module_loader PRIVATE
    IV_CONFIGURED_CLANG_SOURCE_INTROSPECTION_PLUGIN="$<TARGET_FILE:iv_clang_source_introspection_plugin>"
    IV_CONFIGURED_IV_MODULE_FINALIZER="$<TARGET_FILE:iv_module_finalize>")

add_subdirectory(tests)
add_subdirectory(benchmark)

function(iv_force_cxx26 directory)
    get_property(_iv_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_iv_target IN LISTS _iv_targets)
        set_target_properties(${_iv_target} PROPERTIES
            CXX_STANDARD 26
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF)
    endforeach()

    get_property(_iv_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_iv_subdirectory IN LISTS _iv_subdirectories)
        iv_force_cxx26("${_iv_subdirectory}")
    endforeach()
endfunction()
iv_force_cxx26("${CMAKE_SOURCE_DIR}")
