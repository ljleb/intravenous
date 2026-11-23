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
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -Wno-c2y-extensions)
        endif()
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
# nlohmann/json 3.11.3 still uses deprecated std::is_trivial. It is an
# external header-only dependency, so present its
# headers as system headers to consumers instead of suppressing the warning
# for project sources.
set_property(TARGET nlohmann_json PROPERTY SYSTEM ON)

add_subdirectory(build_cache)
set_target_properties(ivctl PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

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
    add_dependencies(${target} intravenous_dsl_pch)
    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=${JUCE_WEB_BROWSER}
        JUCE_USE_CURL=${JUCE_USE_CURL}
        IV_CONFIGURED_IV_BUILDER_LIBRARY="$<TARGET_FILE:iv_builder>"
        IV_CONFIGURED_JUCE_WEB_BROWSER=${JUCE_WEB_BROWSER}
        IV_CONFIGURED_JUCE_USE_CURL=${JUCE_USE_CURL}
        IV_CONFIGURED_CMAKE_COMMAND="${CMAKE_COMMAND}"
        IV_CONFIGURED_BINARY_DIR="${CMAKE_BINARY_DIR}"
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
        iv_builder
        intravenous_module_loader
        intravenous_module_watcher
        intravenous_graph_jit
        intravenous_runtime_iv_module_instances_events
        intravenous_runtime_node_definitions_events
        intravenous_runtime_project_graph_events
        intravenous_runtime_package_pipeline_events
        intravenous_runtime_iv_module_source_introspection_events
        intravenous_runtime_runtime_project_events
        intravenous_runtime_project_persistence_events
        intravenous_runtime_socket_rpc_events
        intravenous_tooling_runtime_objects
        nlohmann_json::nlohmann_json
    )

    if(JUCE_FOUND)
        target_compile_definitions(${target} PRIVATE
            IV_ENABLE_JUCE_VST=1
            JUCE_PLUGINHOST_VST3=1
        )
        target_link_libraries(${target} PRIVATE intravenous_juce_runtime)
    else()
        target_compile_definitions(${target} PRIVATE
            IV_ENABLE_JUCE_VST=0
        )
    endif()

    iv_link_stacktrace_support(${target})
endfunction()

# The default built-ins are one shipped IV package rather than code linked
# directly into iv_builder. Stage only that package's inputs in the build tree.
# Sibling source packages under builtin_packages are optional and must not enter
# the default discovery root. Copying the whole source directory would both
# enable those packages implicitly and copy any old compile database or build/
# tree left behind by an earlier direct-loader invocation.
set(IV_BUILTIN_PACKAGE_SOURCE_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/builtin_packages")
set(IV_BUILTIN_PACKAGE_SEARCH_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/builtin_packages")
set(IV_BUILTIN_PACKAGE_ARTIFACT_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/builtin_package_artifacts"
    CACHE INTERNAL "Executable-owned built-in IV package artifacts")
set(_iv_builtin_package_manifest
    "${IV_BUILTIN_PACKAGE_SEARCH_ROOT}/builtin/iv_package.json")
set(_iv_builtin_package_entry
    "${IV_BUILTIN_PACKAGE_SEARCH_ROOT}/builtin/module.cpp")
set(_iv_builtin_presentations "")
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
foreach(_iv_tool IN ITEMS IV_PRESENTATION_WASM_COMPILER IV_PRESENTATION_WASM_LINKER)
    if(NOT DEFINED ${_iv_tool} AND DEFINED ENV{${_iv_tool}})
        set(${_iv_tool} "$ENV{${_iv_tool}}" CACHE FILEPATH "Presentation WASM build tool")
    endif()
endforeach()
find_program(IV_PRESENTATION_WASM_COMPILER NAMES clang
    HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
find_program(IV_PRESENTATION_WASM_LINKER NAMES wasm-ld
    HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
set(_iv_presentation_builder "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/build_presentation.py")
file(GLOB _iv_presentation_sdk CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/presentation/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/presentation/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/presentation/*.c")
file(GLOB _iv_builtin_presentation_headers CONFIGURE_DEPENDS
    "${IV_BUILTIN_PACKAGE_SOURCE_ROOT}/builtin/presentations/*.hpp")
foreach(_iv_presentation IN ITEMS automation_curve beats click_detector)
    set(_iv_source "${IV_BUILTIN_PACKAGE_SOURCE_ROOT}/builtin/presentations/${_iv_presentation}.cpp")
    set(_iv_destination "${IV_BUILTIN_PACKAGE_SEARCH_ROOT}/builtin/presentations/${_iv_presentation}.wasm")
    add_custom_command(
        OUTPUT "${_iv_destination}"
        COMMAND "${Python3_EXECUTABLE}" "${_iv_presentation_builder}"
            "${_iv_source}" "${_iv_destination}" --clang "${IV_PRESENTATION_WASM_COMPILER}"
            --linker "${IV_PRESENTATION_WASM_LINKER}"
        DEPENDS "${_iv_source}" "${_iv_presentation_builder}" ${_iv_presentation_sdk} ${_iv_builtin_presentation_headers}
        VERBATIM)
    list(APPEND _iv_builtin_presentations "${_iv_destination}")
endforeach()
add_custom_command(
    OUTPUT "${_iv_builtin_package_manifest}" "${_iv_builtin_package_entry}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${IV_BUILTIN_PACKAGE_SEARCH_ROOT}/builtin"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${IV_BUILTIN_PACKAGE_SOURCE_ROOT}/builtin/iv_package.json"
        "${_iv_builtin_package_manifest}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${IV_BUILTIN_PACKAGE_SOURCE_ROOT}/builtin/module.cpp"
        "${_iv_builtin_package_entry}"
    DEPENDS
        "${IV_BUILTIN_PACKAGE_SOURCE_ROOT}/builtin/iv_package.json"
        "${IV_BUILTIN_PACKAGE_SOURCE_ROOT}/builtin/module.cpp"
    VERBATIM
)
add_custom_target(intravenous_builtin_package_assets
    DEPENDS "${_iv_builtin_package_manifest}" "${_iv_builtin_package_entry}" ${_iv_builtin_presentations})

add_library(intravenous_graph_builder OBJECT
    basic_nodes/debug_probe.cpp
    compat.cpp
    throw_site.cpp
    throw_site_hook.cpp
    graph/builder/builder.cpp
    graph/builder/node_refs.cpp
    graph/builder/state.cpp
    graph/reflected_node_description.cpp
    graph_jit/activity_runtime.cpp
    graph_jit/event_conversion_runtime.cpp
    graph_jit/event_retention_runtime.cpp
    module/builder_session.cpp
    module/package_definitions.cpp
    module/source_annotations.cpp
    node/config_storage.cpp
    node/layout.cpp
)
iv_configure_intravenous_target(intravenous_graph_builder PUBLIC)
target_link_libraries(intravenous_graph_builder PRIVATE
    nlohmann_json::nlohmann_json
)

add_library(iv_builder SHARED
    $<TARGET_OBJECTS:intravenous_graph_builder>
)
set_target_properties(iv_builder PROPERTIES OUTPUT_NAME iv_builder)
iv_configure_intravenous_target(iv_builder PUBLIC)
iv_link_stacktrace_support(iv_builder)

include(${CMAKE_CURRENT_SOURCE_DIR}/module/template/PackageCompileFlags.cmake)
set(IV_DSL_PCH "${CMAKE_CURRENT_BINARY_DIR}/iv_dsl.pch")
file(GLOB_RECURSE IV_PACKAGE_PCH_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h" "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp")
add_custom_command(
    OUTPUT "${IV_DSL_PCH}"
    COMMAND "${IV_APPLICATION_ENVIRONMENT_LAUNCHER}" "${CMAKE_CXX_COMPILER}" -std=c++23
        ${IV_PACKAGE_COMPILE_FLAGS} -Xclang -disable-llvm-passes
        "-I${IV_PUBLIC_INCLUDE_ROOT}" -isystem "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
        -DIV_ENABLE_JUCE_VST=0 -x c++-header
        "${CMAKE_CURRENT_SOURCE_DIR}/module/template/module_pch.h"
        -o "${IV_DSL_PCH}"
    DEPENDS "${IV_APPLICATION_ENVIRONMENT_LAUNCHER}"
        "${CMAKE_CURRENT_SOURCE_DIR}/module/template/module_pch.h" ${IV_PACKAGE_PCH_HEADERS}
    VERBATIM)
add_custom_target(intravenous_dsl_pch DEPENDS "${IV_DSL_PCH}")

set(iv_sdk_snapshot "${CMAKE_CURRENT_BINARY_DIR}/runtime-sdk")
file(GLOB iv_sdk_templates CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/module/template/*.cmake")
add_custom_command(OUTPUT "${iv_sdk_snapshot}/.snapshot"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${iv_sdk_snapshot}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}"
        "${iv_sdk_snapshot}/src/intravenous"
    COMMAND "${CMAKE_COMMAND}" -E touch "${iv_sdk_snapshot}/.snapshot"
    DEPENDS "${CMAKE_CURRENT_LIST_FILE}" ${IV_PACKAGE_PCH_HEADERS} ${iv_sdk_templates}
        "${CMAKE_CURRENT_SOURCE_DIR}/presentation/guest_runtime.c"
    VERBATIM)
add_custom_target(intravenous_runtime_sdk DEPENDS "${iv_sdk_snapshot}/.snapshot")
configure_file(module/template/PrepareDevRuntime.cmake.in PrepareDevRuntime.cmake @ONLY)

# This source represents the default project-local module. Its compilation
# database entry is copied to new module sources, where clangd infers the
# command for their module.cpp from this canonical source.
add_library(intravenous_module_source_template_tooling OBJECT EXCLUDE_FROM_ALL
    packages/source_template/module.cpp
)
iv_configure_intravenous_target(intravenous_module_source_template_tooling PUBLIC)
target_link_libraries(intravenous_module_source_template_tooling PRIVATE iv_builder)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(intravenous_module_source_template_tooling PRIVATE
        -Wno-unused-function)
endif()
foreach(iv_implicit_include_dir IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
    if(iv_implicit_include_dir AND EXISTS "${iv_implicit_include_dir}")
        target_compile_options(intravenous_module_source_template_tooling PRIVATE
            "-isystem${iv_implicit_include_dir}")
    endif()
endforeach()

add_library(intravenous_module_loader OBJECT
    module/loader.cpp
)
iv_configure_intravenous_target(intravenous_module_loader PUBLIC)
add_dependencies(intravenous_module_loader intravenous_dsl_pch)
target_include_directories(intravenous_module_loader PRIVATE ${LLVM_INCLUDE_DIRS})
list(JOIN IV_PACKAGE_COMPILE_FLAGS " " IV_PACKAGE_COMPILER_FLAGS_COMMAND_LINE)
target_compile_definitions(intravenous_module_loader PRIVATE
    ${LLVM_DEFINITIONS}
    # ModuleLoader owns the default package toolchain.  Keeping the path on
    # this target means every direct loader user—tests and tools included—gets
    # the one application-built DSL PCH rather than inventing a per-package
    # fallback.
    IV_CONFIGURED_IV_DSL_PCH="${IV_DSL_PCH}"
    IV_CONFIGURED_PACKAGE_COMPILE_FLAGS="${IV_PACKAGE_COMPILER_FLAGS_COMMAND_LINE}"
    IV_CONFIGURED_APPLICATION_ENVIRONMENT_LAUNCHER="${IV_APPLICATION_ENVIRONMENT_LAUNCHER}")
llvm_map_components_to_libnames(IV_PACKAGE_LOADER_LLVM_LIBS
    bitreader
    core
    executionengine
    native
    nativecodegen
    orcdebugging
    orcjit
    orctargetprocess
    passes
    support
    target)
target_link_libraries(intravenous_module_loader PRIVATE
    ${IV_PACKAGE_LOADER_LLVM_LIBS}
    nlohmann_json::nlohmann_json
    intravenous_build_cache
)

add_library(intravenous_module_watcher OBJECT
    module/watcher.cpp
)
iv_configure_intravenous_target(intravenous_module_watcher PUBLIC)

add_library(intravenous_graph_jit OBJECT
    execution/storage/async_capacity_manager.cpp
    execution/background/background_coverage_propagation.cpp
    execution/background/background_evaluation_call_frame.cpp
    execution/background/background_evaluation_transaction.cpp
    execution/background/background_storage_realization.cpp
    execution/graph_executor.cpp
    graph_jit/graph_jit.cpp
    execution/storage/persisted_data_store.cpp
    execution/storage/buffered_storage.cpp
    execution/storage/realtime_persisted_state.cpp
    execution/tick_invocation_frame.cpp
    execution/tick_materialization_snapshot.cpp
    graph_jit/connection_plan.cpp
    graph_jit/bundle_migration.cpp
    graph_jit/connections/port_group_plan.cpp
    graph_jit/connections/realtime_schedule_plan.cpp
    graph_jit/background_storage_plan.cpp
    graph_jit/lowering.cpp
    graph_jit/lowering/event_port_plan.cpp
    graph_jit/lowering/sample_port_plan.cpp
    graph_jit/lowering_plan.cpp
    graph_jit/port_realization.cpp
    graph_jit/realtime_root.cpp
    graph_jit/realtime/root_plan.cpp
    graph_jit/realtime_code_compatibility.cpp
    graph_jit/realtime_port_state_transition.cpp
    graph_jit/region_specialization.cpp
    graph_jit/region_specialization_emit.cpp
    graph_jit/sample_storage_plan.cpp
    graph_jit/transient_arena_plan.cpp
)
iv_configure_intravenous_target(intravenous_graph_jit PUBLIC)
target_include_directories(intravenous_graph_jit PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(intravenous_graph_jit PRIVATE
    ${LLVM_DEFINITIONS}
    IV_CONFIGURED_IV_BUILDER_LIBRARY="$<TARGET_FILE:iv_builder>"
)
llvm_map_components_to_libnames(IV_GRAPH_JIT_LLVM_LIBS
    bitreader
    core
    executionengine
    linker
    native
    nativecodegen
    orcdebugging
    orcjit
    orctargetprocess
    passes
    support
    target)
target_link_libraries(intravenous_graph_jit PRIVATE
    ${IV_GRAPH_JIT_LLVM_LIBS}
)
add_dependencies(intravenous_graph_jit iv_builder)

iv_add_linker_event_object(
    intravenous_runtime_iv_module_instances_events
    project/iv_module_instances_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_node_definitions_events
    packages/node_definitions_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_package_pipeline_events
    packages/package_pipeline_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_project_graph_events
    project/project_graph_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_iv_module_source_introspection_events
    packages/iv_module_source_introspection_linker_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_runtime_project_events
    project/runtime_project_linker_events.cpp
)
iv_add_linker_event_object(
    intravenous_runtime_project_persistence_events
    project/persistence/project_persistence_events.cpp
)
target_link_libraries(intravenous_runtime_project_persistence_events PRIVATE
    nlohmann_json::nlohmann_json
)
iv_add_linker_event_object(
    intravenous_runtime_socket_rpc_events
    rpc/socket_rpc_linker_events.cpp
)
target_link_libraries(intravenous_runtime_socket_rpc_events PRIVATE
    nlohmann_json::nlohmann_json
)
target_link_libraries(intravenous_runtime_runtime_project_events PRIVATE
    nlohmann_json::nlohmann_json
)

add_library(intravenous_tooling_runtime_objects OBJECT
    query/tag_query_completion.cpp
    query/tag_query_engine.cpp
    query/tag_query_parser.cpp
    query/tag_query_program.cpp
    query/tag_query_tokenizer.cpp
    audio/system_audio_devices.cpp
    audio/audio_input_synchronizer.cpp
    app/config.cpp
    packages/node_definitions.cpp
    packages/node_definition_search.cpp
    app/bridges/node_definitions_project_graph_bridge.cpp
    app/bridges/package_definitions_node_definitions_bridge.cpp
    app/bridges/package_watcher_package_definitions_bridge.cpp
    app/bridges/package_watcher_package_jit_bridge.cpp
    app/bridges/package_watcher_project_graph_bridge.cpp
    app/bridges/package_watcher_node_instances_bridge.cpp
    app/bridges/package_watcher_service_bridge.cpp
    project/node_instances.cpp
    app/bridges/node_instances_iv_module_source_introspection_bridge.cpp
    project/configuration_expression.cpp
    project/graph_connections.cpp
    project/persistence/project_connection_json.cpp
    project/project_node_path_resolver.cpp
    project/project_graph.cpp
    observations/observations.cpp
    app/bridges/observation_bridges.cpp
    app/bridges/project_graph_system_audio_devices_bridge.cpp
    app/bridges/project_graph_package_jit_bridge.cpp
    app/bridges/project_graph_graph_connections_bridge.cpp
    app/bridges/project_graph_graph_executor_bridge.cpp
    app/bridges/project_graph_graph_jit_bridge.cpp
    app/bridges/project_graph_iv_module_source_introspection_bridge.cpp
    app/bridges/project_graph_node_instances_bridge.cpp
    app/bridges/project_graph_package_definitions_bridge.cpp
    packages/package_definitions.cpp
    packages/presentation_artifacts.cpp
    packages/package_jit.cpp
    packages/package_watcher.cpp
    packages/package_discovery_watcher.cpp
    packages/package_watcher_service.cpp
    packages/iv_module_source_introspection.cpp
    packages/iv_module_source_introspection_events.cpp
    project/persistence/project_persistence.cpp
    project/persistence/project_persistence_builder.cpp
    project/persistence/project_autosave.cpp
    project/runtime_project_events.cpp
    app/bridges/project_persistence_system_audio_devices_bridge.cpp
    app/bridges/project_persistence_project_autosave_bridge.cpp
    app/bridges/project_persistence_project_graph_bridge.cpp
    app/bridges/project_persistence_package_jit_bridge.cpp
    app/bridges/socket_rpc_system_audio_devices_bridge.cpp
    app/bridges/socket_rpc_graph_executor_bridge.cpp
    app/bridges/socket_rpc_iv_module_source_introspection_bridge.cpp
    app/bridges/socket_rpc_node_instances_bridge.cpp
    app/bridges/socket_rpc_project_graph_bridge.cpp
    app/bridges/socket_rpc_package_definitions_bridge.cpp
    app/bridges/socket_rpc_package_watcher_bridge.cpp
    app/bridges/socket_rpc_project_autosave_bridge.cpp
    app/bridges/socket_rpc_project_persistence_bridge.cpp
    rpc/socket_rpc_json_serialization.cpp
    rpc/socket_rpc_requests.cpp
    rpc/socket_rpc_response_builders.cpp
    rpc/socket_rpc_routes.cpp
    rpc/socket_rpc_server.cpp
    app/startup_config.cpp
    project/uuid.cpp
)
iv_configure_intravenous_target(intravenous_tooling_runtime_objects PUBLIC)
target_include_directories(intravenous_tooling_runtime_objects SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
list(JOIN IV_PACKAGE_COMPILE_FLAGS " " IV_PACKAGE_COMPILE_FLAGS_COMMAND_LINE)
target_compile_definitions(intravenous_tooling_runtime_objects PRIVATE
    IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_COMPILE_FLAGS="${IV_PACKAGE_COMPILE_FLAGS_COMMAND_LINE}"
    IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_DSL_PCH="${IV_DSL_PCH}"
    IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_INCLUDE_DIR="${IV_PUBLIC_INCLUDE_ROOT}"
    IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_THIRD_PARTY_INCLUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)
target_link_libraries(intravenous_tooling_runtime_objects PRIVATE
    nlohmann_json::nlohmann_json
    intravenous_build_cache
    LLVMSupport
)

if(JUCE_FOUND)
    iv_configure_juce()

    # JUCE module targets expose their implementation files as interface
    # sources. Consume them once here, rather than once per host executable.
    add_library(intravenous_juce_runtime STATIC
        juce/vst_runtime.cpp
    )
    set_target_properties(intravenous_juce_runtime PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
    iv_configure_intravenous_target(intravenous_juce_runtime PUBLIC)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(intravenous_juce_runtime PRIVATE
            -Wno-array-bounds
            -Wno-unnecessary-virtual-specifier
            -Wno-unused-template
        )
    endif()
    target_compile_definitions(intravenous_juce_runtime PRIVATE
        IV_ENABLE_JUCE_VST=1
        JUCE_PLUGINHOST_VST3=1
    )
    target_link_libraries(intravenous_juce_runtime PRIVATE
        ${IV_JUCE_AUDIO_PROCESSORS_TARGET}
        ${IV_JUCE_AUDIO_DEVICES_TARGET}
        ${IV_JUCE_EVENTS_TARGET}
        ${IV_JUCE_CORE_TARGET}
    )

    # This library remains the runtime-symbol provider for JUCE/VST node
    # modules. The builder boundary must not remove that opt-in capability.
    target_compile_definitions(iv_builder PUBLIC
        IV_ENABLE_JUCE_VST=1
        JUCE_PLUGINHOST_VST3=1
    )
    target_link_libraries(iv_builder PRIVATE intravenous_juce_runtime)

    target_compile_definitions(intravenous_module_loader PRIVATE
        IV_CONFIGURED_ENABLE_JUCE_VST=1
        IV_CONFIGURED_JUCE_DIR="${JUCE_DIR}"
        IV_CONFIGURED_JUCE_MODULES_DIR="${IV_JUCE_MODULES_DIR}"
    )
else()
    target_compile_definitions(iv_builder PUBLIC
        IV_ENABLE_JUCE_VST=0
    )

    target_compile_definitions(intravenous_module_loader PRIVATE
        IV_CONFIGURED_ENABLE_JUCE_VST=0
        IV_CONFIGURED_JUCE_DIR=""
        IV_CONFIGURED_JUCE_MODULES_DIR=""
    )
endif()

target_compile_definitions(intravenous_module_loader PRIVATE
    IV_CONFIGURED_IV_BUILDER_LIBRARY="$<TARGET_FILE:iv_builder>"
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
    app/app.cpp
)
set_target_properties(intravenous PROPERTIES BUILD_RPATH_USE_ORIGIN TRUE)

# miniaudio loads libpulse by name. Keep its library directory available
# when the server runs outside the development shell.
find_library(IV_PULSE_LIBRARY NAMES pulse libpulse.so.0)
if(IV_PULSE_LIBRARY)
    get_filename_component(iv_pulse_library_dir "${IV_PULSE_LIBRARY}" DIRECTORY)
    set_property(TARGET intravenous APPEND PROPERTY BUILD_RPATH "${iv_pulse_library_dir}")
endif()

add_executable(intravenous-client-helper client_native_helper.cpp)
target_link_libraries(intravenous-client-helper PRIVATE nlohmann_json::nlohmann_json)
find_package(X11 QUIET)
if(X11_FOUND)
    target_link_libraries(intravenous-client-helper PRIVATE X11::X11)
    target_compile_definitions(intravenous-client-helper PRIVATE IV_CLIENT_HELPER_X11)
    set(iv_client_xfixes_target X11::Xfixes)
    if(NOT TARGET ${iv_client_xfixes_target})
        # The Nix development shell can expose XFixes through pkg-config
        # even when FindX11's header and library searches do not see it.
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(IV_CLIENT_XFIXES QUIET IMPORTED_TARGET xfixes)
            set(iv_client_xfixes_target PkgConfig::IV_CLIENT_XFIXES)
        endif()
    endif()
    if(TARGET ${iv_client_xfixes_target})
        target_link_libraries(intravenous-client-helper PRIVATE ${iv_client_xfixes_target})
        target_compile_definitions(intravenous-client-helper PRIVATE IV_CLIENT_HELPER_XFIXES)
    endif()
endif()
add_dependencies(intravenous intravenous-client-helper)

iv_apply_common_cxx_settings(intravenous PRIVATE)
target_compile_definitions(intravenous PRIVATE
    IV_CONFIGURED_APPLICATION_ENVIRONMENT_LAUNCHER="${IV_APPLICATION_ENVIRONMENT_LAUNCHER}"
    IV_CONFIGURED_APPLICATION_ENVIRONMENT_ID="${IV_APPLICATION_ENVIRONMENT_ID}")

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
add_dependencies(intravenous intravenous_dsl_pch intravenous_runtime_sdk ivctl)
# The launcher bundles these staged inputs with the executable. Relink even
# when an SDK or builtin-only change leaves all server objects unchanged.
set_property(TARGET intravenous APPEND PROPERTY LINK_DEPENDS
    "${iv_sdk_snapshot}/.snapshot"
    "${_iv_builtin_package_manifest}"
    "${_iv_builtin_package_entry}")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/.intravenous_defaults"
"# Generated by CMake for the Intravenous server build
c_compiler=${CMAKE_C_COMPILER}
cxx_compiler=${CMAKE_CXX_COMPILER}
cmake_program=${CMAKE_COMMAND}
cmake_generator=${CMAKE_GENERATOR}
make_program=${CMAKE_MAKE_PROGRAM}
juce_dir=${JUCE_DIR}
iv_package_pch=${IV_DSL_PCH}
iv_builtin_package_root=${IV_BUILTIN_PACKAGE_SEARCH_ROOT}
iv_builtin_package_artifact_root=${IV_BUILTIN_PACKAGE_ARTIFACT_ROOT}
sample_rate=48000
block_size=256
")
