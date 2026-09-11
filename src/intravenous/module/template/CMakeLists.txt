cmake_minimum_required(VERSION 3.20)
project(iv_package LANGUAGES CXX)
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

# Cross-package registered IDs resolve during graph configuration, never by
# merging another package's implementation files into this compile target.
iv_add_package(iv_package ENABLE_JUCE)
