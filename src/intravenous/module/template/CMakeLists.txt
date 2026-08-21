cmake_minimum_required(VERSION 3.20)
project(iv_runtime_module LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED JUCE_WEB_BROWSER)
    set(JUCE_WEB_BROWSER 0)
endif()
if(NOT DEFINED JUCE_USE_CURL)
    set(JUCE_USE_CURL 0)
endif()
add_compile_definitions(
    JUCE_WEB_BROWSER=${JUCE_WEB_BROWSER}
    JUCE_USE_CURL=${JUCE_USE_CURL})

include("${CMAKE_CURRENT_LIST_DIR}/ModuleSupport.cmake")

# The authored entry is represented by its rewritten definition-bearing import
# and is included by the generated export TU. It is not compiled as a separate
# source file.
iv_add_runtime_module(iv_runtime_module ENABLE_JUCE)
