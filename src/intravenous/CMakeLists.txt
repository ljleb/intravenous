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

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

set(IV_PUBLIC_INCLUDE_ROOT "${CMAKE_SOURCE_DIR}/src")

function(iv_configure_intravenous_target target defs_visibility)
    iv_apply_common_cxx_settings(${target} ${defs_visibility})
    target_include_directories(${target} PUBLIC
        ${IV_PUBLIC_INCLUDE_ROOT}
    )
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_include_directories(${target} SYSTEM PUBLIC
        third_party/
    )
endfunction()

function(iv_add_linker_event_object target source)
    add_library(${target} OBJECT ${source})
    iv_configure_intravenous_target(${target} PUBLIC)
endfunction()

function(iv_add_host_runtime_objects target)
    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=${JUCE_WEB_BROWSER}
        JUCE_USE_CURL=${JUCE_USE_CURL}
        IV_CONFIGURED_IV_MODULE_SHARED_LIBRARY="$<TARGET_FILE:iv_module_shared>"
        IV_CONFIGURED_JUCE_WEB_BROWSER=${JUCE_WEB_BROWSER}
        IV_CONFIGURED_JUCE_USE_CURL=${JUCE_USE_CURL}
        IV_CONFIGURED_CMAKE_COMMAND="${CMAKE_COMMAND}"
        IV_CONFIGURED_CMAKE_GENERATOR="${CMAKE_GENERATOR}"
        IV_CONFIGURED_MAKE_PROGRAM="${CMAKE_MAKE_PROGRAM}"
        IV_CONFIGURED_C_COMPILER="${CMAKE_C_COMPILER}"
        IV_CONFIGURED_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    )

    if(JUCE_FOUND)
        target_compile_definitions(${target} PRIVATE
            IV_CONFIGURED_ENABLE_JUCE_VST=1
            IV_CONFIGURED_JUCE_DIR="${JUCE_DIR}"
            IV_CONFIGURED_JUCE_MODULES_DIR="${IV_JUCE_MODULES_DIR}"
        )
    else()
        target_compile_definitions(${target} PRIVATE
            IV_CONFIGURED_ENABLE_JUCE_VST=0
            IV_CONFIGURED_JUCE_DIR=""
            IV_CONFIGURED_JUCE_MODULES_DIR=""
        )
    endif()

    target_link_libraries(${target} PRIVATE
        intravenous_graph_builder
        intravenous_module_loader
        intravenous_module_watcher
        intravenous_runtime_iv_module_instances_events
        intravenous_runtime_iv_module_definitions_events
        intravenous_runtime_iv_module_reload_events
        intravenous_runtime_iv_module_source_introspection_events
        intravenous_runtime_graph_input_lanes_events
        intravenous_runtime_graph_input_lanes_execution_edges_events
        intravenous_runtime_lane_filters_events
        intravenous_runtime_lane_views_events
        intravenous_runtime_runtime_project_events
        intravenous_runtime_socket_rpc_events
        intravenous_runtime_timeline_events
        intravenous_runtime_timeline_execution_events
        intravenous_runtime_iv_module_instances_execution_events
        intravenous_tooling_runtime_objects
        nlohmann_json::nlohmann_json
    )

    if(JUCE_FOUND)
        target_sources(${target} PRIVATE
            ${CMAKE_SOURCE_DIR}/src/intravenous/juce/vst_runtime.cpp
        )
        target_compile_definitions(${target} PRIVATE
            IV_ENABLE_JUCE_VST=1
            JUCE_PLUGINHOST_VST3=1
        )
        if(TARGET juce::juce_audio_processors)
            target_link_libraries(${target} PRIVATE
                juce::juce_audio_processors
                juce::juce_audio_devices
                juce::juce_events
                juce::juce_core
            )
        elseif(TARGET JUCE::juce_audio_processors)
            target_link_libraries(${target} PRIVATE
                JUCE::juce_audio_processors
                JUCE::juce_audio_devices
                JUCE::juce_events
                JUCE::juce_core
            )
        endif()
    else()
        target_compile_definitions(${target} PRIVATE
            IV_ENABLE_JUCE_VST=0
        )
    endif()

    iv_link_stacktrace_support(${target})
endfunction()

add_library(intravenous_graph_builder OBJECT
    graph/builder/stored_node.cpp
    graph/builder/topology.cpp
    graph/builder/public_ports.cpp
    graph/builder/subgraphs.cpp
    graph/builder/connections.cpp
    graph/builder/detach.cpp
    graph/builder/annotations.cpp
    graph/builder/embedder.cpp
    graph/builder/graph.cpp
    graph/builder/finalize.cpp
    graph/builder/names.cpp
    graph/builder/metadata.cpp
)
iv_configure_intravenous_target(intravenous_graph_builder PUBLIC)

add_library(iv_module_shared SHARED
    $<TARGET_OBJECTS:intravenous_graph_builder>
)
iv_configure_intravenous_target(iv_module_shared PUBLIC)
iv_link_stacktrace_support(iv_module_shared)

add_library(intravenous_module_loader OBJECT
    module/loader.cpp
)
iv_configure_intravenous_target(intravenous_module_loader PUBLIC)

add_library(intravenous_module_watcher OBJECT
    module/watcher.cpp
)
iv_configure_intravenous_target(intravenous_module_watcher PUBLIC)

iv_add_linker_event_object(
    intravenous_runtime_iv_module_instances_events
    runtime/iv_module_instances_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_iv_module_definitions_events
    runtime/iv_module_definitions_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_iv_module_reload_events
    runtime/iv_module_reload_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_iv_module_source_introspection_events
    runtime/iv_module_source_introspection_linker_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_graph_input_lanes_events
    runtime/graph_input_lanes_linker_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_lane_filters_events
    runtime/lane_filters_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_lane_views_events
    runtime/lane_views_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_runtime_project_events
    runtime/runtime_project_linker_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_socket_rpc_events
    runtime/socket_rpc_linker_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_timeline_events
    runtime/timeline_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_graph_input_lanes_execution_edges_events
    runtime/graph_input_lanes_execution_edges_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_timeline_execution_events
    runtime/timeline_execution_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_iv_module_instances_execution_events
    runtime/iv_module_instances_execution_events.cpp
)

add_library(intravenous_tooling_runtime_objects OBJECT
    runtime/config.cpp
    runtime/graph_input_lanes.cpp
    runtime/graph_input_lanes_events.cpp
    runtime/graph_input_lanes_iv_module_instances_execution_bridge.cpp
    runtime/graph_input_lanes_timeline_bridge.cpp
    runtime/iv_module_definitions.cpp
    runtime/iv_module_definitions_iv_module_instances_bridge.cpp
    runtime/iv_module_definitions_iv_module_reload_bridge.cpp
    runtime/iv_module_definitions_iv_module_source_introspection_bridge.cpp
    runtime/iv_module_instances.cpp
    runtime/iv_module_instances_execution.cpp
    runtime/iv_module_instances_execution_task_runner_bridge.cpp
    runtime/iv_module_instances_iv_module_definitions_bridge.cpp
    runtime/iv_module_instances_iv_module_instances_execution_bridge.cpp
    runtime/iv_module_instances_graph_input_lanes_bridge.cpp
    runtime/iv_module_reload.cpp
    runtime/iv_module_reload_iv_module_definitions_bridge.cpp
    runtime/lane_filters.cpp
    runtime/lane_filters_lane_views_bridge.cpp
    runtime/lane_views.cpp
    runtime/lane_view_service.cpp
    runtime/iv_module_source_introspection.cpp
    runtime/iv_module_source_introspection_events.cpp
    runtime/iv_module_source_introspection_graph_input_lanes_bridge.cpp
    runtime/runtime_project_events.cpp
    runtime/socket_rpc_lane_views_bridge.cpp
    runtime/socket_rpc_iv_module_source_introspection_bridge.cpp
    runtime/socket_rpc_iv_module_instances_bridge.cpp
    runtime/socket_rpc_timeline_execution_bridge.cpp
    runtime/socket_rpc_notification_bridge.cpp
    runtime/socket_rpc_requests.cpp
    runtime/socket_rpc_response_builders.cpp
    runtime/socket_rpc_server.cpp
    runtime/startup_config.cpp
    runtime/task_runner.cpp
    runtime/timeline_execution.cpp
    runtime/timeline_execution_task_runner_bridge.cpp
    runtime/timeline_lane_filters_bridge.cpp
    runtime/timeline_timeline_execution_bridge.cpp
    query/lane_query_engine.cpp
    query/lane_query_parser.cpp
    query/lane_query_program.cpp
    query/lane_query_tokenizer.cpp
)
iv_configure_intravenous_target(intravenous_tooling_runtime_objects PUBLIC)
target_link_libraries(intravenous_tooling_runtime_objects PRIVATE
    nlohmann_json::nlohmann_json
)

if(JUCE_FOUND)
    iv_configure_juce()
    
    target_sources(iv_module_shared PRIVATE
        juce/vst_runtime.cpp
    )

    target_compile_definitions(iv_module_shared PUBLIC
        IV_ENABLE_JUCE_VST=1
        JUCE_PLUGINHOST_VST3=1
    )

    target_link_libraries(iv_module_shared PRIVATE
        ${IV_JUCE_AUDIO_PROCESSORS_TARGET}
        ${IV_JUCE_AUDIO_DEVICES_TARGET}
        ${IV_JUCE_EVENTS_TARGET}
        ${IV_JUCE_CORE_TARGET}
    )

    target_compile_definitions(intravenous_module_loader PRIVATE
        IV_CONFIGURED_ENABLE_JUCE_VST=1
        IV_CONFIGURED_JUCE_DIR="${JUCE_DIR}"
        IV_CONFIGURED_JUCE_MODULES_DIR="${IV_JUCE_MODULES_DIR}"
    )
else()
    target_compile_definitions(iv_module_shared PUBLIC
        IV_ENABLE_JUCE_VST=0
    )

    target_compile_definitions(intravenous_module_loader PRIVATE
        IV_CONFIGURED_ENABLE_JUCE_VST=0
        IV_CONFIGURED_JUCE_DIR=""
        IV_CONFIGURED_JUCE_MODULES_DIR=""
    )
endif()

target_compile_definitions(intravenous_module_loader PRIVATE
    IV_CONFIGURED_IV_MODULE_SHARED_LIBRARY="$<TARGET_FILE:iv_module_shared>"
    IV_CONFIGURED_JUCE_WEB_BROWSER=${JUCE_WEB_BROWSER}
    IV_CONFIGURED_JUCE_USE_CURL=${JUCE_USE_CURL}
    IV_CONFIGURED_CMAKE_COMMAND="${CMAKE_COMMAND}"
    IV_CONFIGURED_CMAKE_GENERATOR="${CMAKE_GENERATOR}"
    IV_CONFIGURED_MAKE_PROGRAM="${CMAKE_MAKE_PROGRAM}"
    IV_CONFIGURED_C_COMPILER="${CMAKE_C_COMPILER}"
    IV_CONFIGURED_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
)

add_executable(intravenous
    third_party/miniaudio/miniaudio.c
    main.cpp
    runtime/app.cpp
)

iv_apply_common_cxx_settings(intravenous PRIVATE)

target_include_directories(intravenous PRIVATE
    ${IV_PUBLIC_INCLUDE_ROOT}
)

target_include_directories(intravenous PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(intravenous SYSTEM PRIVATE
    third_party/
)

iv_add_host_runtime_objects(intravenous)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/.intravenous_defaults"
"# Generated by CMake for the Intravenous server build
c_compiler=${CMAKE_C_COMPILER}
cxx_compiler=${CMAKE_CXX_COMPILER}
cmake_program=${CMAKE_COMMAND}
cmake_generator=${CMAKE_GENERATOR}
make_program=${CMAKE_MAKE_PROGRAM}
juce_dir=${JUCE_DIR}
sample_rate=48000
block_size=256
compiled_sample_cache_chunk_size_multiplier=16
")
