cmake_minimum_required(VERSION 3.20)

project(iv_runtime_module LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${IV_CORE_INCLUDE_DIR}/module/template/ModuleSupport.cmake)

iv_add_runtime_module(iv_runtime_module
    SOURCES
        ${IV_MODULE_ENTRY_FILE}
    ENABLE_JUCE
)
