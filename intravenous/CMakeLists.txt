cmake_minimum_required(VERSION 3.20)

project(intravenous LANGUAGES C CXX)

include(FetchContent)

# JUCE uses these as compile-time module config macros.
# Keep them centralized at directory scope so JUCE-related targets pick them up.
add_compile_definitions(
    JUCE_WEB_BROWSER=${JUCE_WEB_BROWSER}
    JUCE_USE_CURL=${JUCE_USE_CURL}
)

function(iv_apply_common_cxx_settings target defs_visibility)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
        )
        target_compile_definitions(${target} ${defs_visibility}
            _CRT_SECURE_NO_WARNINGS
            _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-deprecated-declarations
        )
    endif()
endfunction()

function(iv_link_stacktrace_support target)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_link_libraries(${target} PUBLIC stdc++exp)
    endif()
endfunction()

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

function(iv_configure_juce)
    set(IV_JUCE_AUDIO_PROCESSORS_TARGET "")
    set(IV_JUCE_AUDIO_DEVICES_TARGET "")
    set(IV_JUCE_EVENTS_TARGET "")
    set(IV_JUCE_CORE_TARGET "")
    set(IV_JUCE_MODULES_DIR "")

    if(TARGET juce::juce_audio_processors)
        set(IV_JUCE_AUDIO_PROCESSORS_TARGET juce::juce_audio_processors)
        set(IV_JUCE_AUDIO_DEVICES_TARGET juce::juce_audio_devices)
        set(IV_JUCE_EVENTS_TARGET juce::juce_events)
        set(IV_JUCE_CORE_TARGET juce::juce_core)
    elseif(TARGET JUCE::juce_audio_processors)
        set(IV_JUCE_AUDIO_PROCESSORS_TARGET JUCE::juce_audio_processors)
        set(IV_JUCE_AUDIO_DEVICES_TARGET JUCE::juce_audio_devices)
        set(IV_JUCE_EVENTS_TARGET JUCE::juce_events)
        set(IV_JUCE_CORE_TARGET JUCE::juce_core)
    else()
        message(FATAL_ERROR "JUCE was found but no supported JUCE CMake targets are available")
    endif()

    if(DEFINED JUCE_MODULES_DIR AND EXISTS "${JUCE_MODULES_DIR}")
        set(IV_JUCE_MODULES_DIR "${JUCE_MODULES_DIR}")
    elseif(DEFINED JUCE_SOURCE_DIR AND EXISTS "${JUCE_SOURCE_DIR}/modules")
        set(IV_JUCE_MODULES_DIR "${JUCE_SOURCE_DIR}/modules")
    elseif(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/modules")
        set(IV_JUCE_MODULES_DIR "${JUCE_DIR}/modules")
    elseif(DEFINED JUCE_DIR)
        file(GLOB IV_JUCE_MODULES_CANDIDATES LIST_DIRECTORIES true "${JUCE_DIR}/include/JUCE-*/modules")
        list(LENGTH IV_JUCE_MODULES_CANDIDATES IV_JUCE_MODULES_CANDIDATES_LENGTH)
        if(IV_JUCE_MODULES_CANDIDATES_LENGTH GREATER 0)
            list(GET IV_JUCE_MODULES_CANDIDATES 0 IV_JUCE_MODULES_DIR)
        endif()
    endif()

    set(IV_JUCE_AUDIO_PROCESSORS_TARGET "${IV_JUCE_AUDIO_PROCESSORS_TARGET}" PARENT_SCOPE)
    set(IV_JUCE_AUDIO_DEVICES_TARGET "${IV_JUCE_AUDIO_DEVICES_TARGET}" PARENT_SCOPE)
    set(IV_JUCE_EVENTS_TARGET "${IV_JUCE_EVENTS_TARGET}" PARENT_SCOPE)
    set(IV_JUCE_CORE_TARGET "${IV_JUCE_CORE_TARGET}" PARENT_SCOPE)
    set(IV_JUCE_MODULES_DIR "${IV_JUCE_MODULES_DIR}" PARENT_SCOPE)
endfunction()

add_library(intravenous_module_runtime STATIC
    module/loader.cpp
    module/watcher.cpp
    runtime/iv_module_instances_events.cpp
    runtime/lane_ref.cpp
)

iv_apply_common_cxx_settings(intravenous_module_runtime PUBLIC)
iv_link_stacktrace_support(intravenous_module_runtime)

target_include_directories(intravenous_module_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(intravenous_module_runtime SYSTEM PUBLIC
    third_party/
)

if(JUCE_FOUND)
    iv_configure_juce()

    target_sources(intravenous_module_runtime PRIVATE
        juce/vst_runtime.cpp
    )

    target_compile_definitions(intravenous_module_runtime PUBLIC
        IV_ENABLE_JUCE_VST=1
        JUCE_PLUGINHOST_VST3=1
        IV_CONFIGURED_ENABLE_JUCE_VST=1
        IV_CONFIGURED_JUCE_MODULES_DIR="${IV_JUCE_MODULES_DIR}"
    )

    if(DEFINED JUCE_DIR AND NOT JUCE_DIR STREQUAL "")
        target_compile_definitions(intravenous_module_runtime PUBLIC
            IV_CONFIGURED_JUCE_DIR="${JUCE_DIR}"
        )
    else()
        target_compile_definitions(intravenous_module_runtime PUBLIC
            IV_CONFIGURED_JUCE_DIR=""
        )
    endif()

    target_link_libraries(intravenous_module_runtime PRIVATE
        ${IV_JUCE_AUDIO_PROCESSORS_TARGET}
        ${IV_JUCE_AUDIO_DEVICES_TARGET}
        ${IV_JUCE_EVENTS_TARGET}
        ${IV_JUCE_CORE_TARGET}
    )
else()
    target_compile_definitions(intravenous_module_runtime PUBLIC
        IV_ENABLE_JUCE_VST=0
        IV_CONFIGURED_ENABLE_JUCE_VST=0
        IV_CONFIGURED_JUCE_DIR=""
        IV_CONFIGURED_JUCE_MODULES_DIR=""
    )
endif()

target_compile_definitions(intravenous_module_runtime PUBLIC
    IV_CONFIGURED_CORE_RUNTIME_LIBRARY="$<TARGET_LINKER_FILE:intravenous_module_runtime>"
    IV_CONFIGURED_JUCE_WEB_BROWSER=${JUCE_WEB_BROWSER}
    IV_CONFIGURED_JUCE_USE_CURL=${JUCE_USE_CURL}
    IV_CONFIGURED_CMAKE_COMMAND="${CMAKE_COMMAND}"
    IV_CONFIGURED_CMAKE_GENERATOR="${CMAKE_GENERATOR}"
    IV_CONFIGURED_MAKE_PROGRAM="${CMAKE_MAKE_PROGRAM}"
    IV_CONFIGURED_C_COMPILER="${CMAKE_C_COMPILER}"
    IV_CONFIGURED_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
)

add_library(intravenous_tooling_runtime STATIC
    runtime/config.cpp
    runtime/graph_input_lanes.cpp
    runtime/graph_input_lanes_events.cpp
    runtime/graph_input_lanes_lane_views_bridge.cpp
    runtime/graph_input_lanes_timeline_bridge.cpp
    runtime/iv_module_definitions.cpp
    runtime/iv_module_definitions_events.cpp
    runtime/iv_module_definitions_iv_module_instances_bridge.cpp
    runtime/iv_module_definitions_iv_module_reload_bridge.cpp
    runtime/iv_module_definitions_project_introspection_bridge.cpp
    runtime/iv_module_instances.cpp
    runtime/iv_module_instances_iv_module_definitions_bridge.cpp
    runtime/iv_module_instances_graph_input_lanes_bridge.cpp
    runtime/iv_module_reload.cpp
    runtime/iv_module_reload_events.cpp
    runtime/iv_module_reload_iv_module_definitions_bridge.cpp
    runtime/lane_views.cpp
    runtime/lane_views_events.cpp
    runtime/lane_views_timeline_bridge.cpp
    runtime/lane_view_service.cpp
    runtime/project_introspection.cpp
    runtime/project_introspection_events.cpp
    runtime/project_introspection_graph_input_lanes_bridge.cpp
    runtime/runtime_project_events.cpp
    runtime/socket_rpc_lane_views_bridge.cpp
    runtime/socket_rpc_project_introspection_bridge.cpp
    runtime/socket_rpc_iv_module_instances_bridge.cpp
    runtime/socket_rpc_notification_bridge.cpp
    runtime/socket_rpc_requests.cpp
    runtime/socket_rpc_response_builders.cpp
    runtime/socket_rpc_server.cpp
    runtime/startup_config.cpp
    runtime/timeline_events.cpp
)

iv_apply_common_cxx_settings(intravenous_tooling_runtime PUBLIC)
iv_link_stacktrace_support(intravenous_tooling_runtime)

target_include_directories(intravenous_tooling_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(intravenous_tooling_runtime SYSTEM PUBLIC
    third_party/
)

target_link_libraries(intravenous_tooling_runtime PUBLIC
    intravenous_module_runtime
)

target_link_libraries(intravenous_tooling_runtime PRIVATE
    nlohmann_json::nlohmann_json
)

add_library(intravenous_json_rpc_server INTERFACE)
target_link_libraries(intravenous_json_rpc_server INTERFACE
    intravenous_tooling_runtime
)

add_executable(intravenous
    third_party/miniaudio/miniaudio.c
    main.cpp
    runtime/app.cpp
)

iv_apply_common_cxx_settings(intravenous PRIVATE)

target_include_directories(intravenous PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(intravenous SYSTEM PRIVATE
    third_party/
)

target_link_libraries(intravenous PRIVATE
    intravenous_module_runtime
    intravenous_json_rpc_server
    intravenous_tooling_runtime
)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/.intravenous_defaults"
"# Generated by CMake for the Intravenous server build
cCompiler=${CMAKE_C_COMPILER}
cxxCompiler=${CMAKE_CXX_COMPILER}
cmakeProgram=${CMAKE_COMMAND}
cmakeGenerator=${CMAKE_GENERATOR}
makeProgram=${CMAKE_MAKE_PROGRAM}
juceDir=${JUCE_DIR}
")
