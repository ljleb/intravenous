cmake_minimum_required(VERSION 3.20)

project(iv_runtime_module LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED JUCE_WEB_BROWSER)
    set(JUCE_WEB_BROWSER 0)
endif()
if(NOT DEFINED JUCE_USE_CURL)
    set(JUCE_USE_CURL 0)
endif()

# JUCE uses these as compile-time module config macros.
add_compile_definitions(
    JUCE_WEB_BROWSER=${JUCE_WEB_BROWSER}
    JUCE_USE_CURL=${JUCE_USE_CURL}
)

include("${CMAKE_CURRENT_LIST_DIR}/ModuleSupport.cmake")

iv_add_runtime_module(iv_runtime_module
    SOURCES
        ${IV_MODULE_ENTRY_FILE}
    ENABLE_JUCE
)
