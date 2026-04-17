include_guard(GLOBAL)

include(${IV_CORE_INCLUDE_DIR}/module/template/JuceSupport.cmake)
include(${IV_CORE_INCLUDE_DIR}/module/template/SourceSpanRewrite.cmake)

function(iv_configure_core_runtime_import)
    set(IV_CORE_RUNTIME_LIBRARY "${IV_CORE_RUNTIME_LIBRARY}" CACHE FILEPATH "Path to the built intravenous_module_runtime library")

    if(NOT IV_CORE_RUNTIME_LIBRARY OR NOT EXISTS "${IV_CORE_RUNTIME_LIBRARY}")
        return()
    endif()

    if(NOT TARGET intravenous_module_runtime)
        set(_iv_core_runtime_link_libraries "")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            # std::stacktrace on libstdc++ requires stdc++exp at final link.
            list(APPEND _iv_core_runtime_link_libraries stdc++exp)
        endif()

        add_library(intravenous_module_runtime STATIC IMPORTED GLOBAL)
        set_target_properties(intravenous_module_runtime PROPERTIES
            IMPORTED_LOCATION "${IV_CORE_RUNTIME_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${IV_CORE_INCLUDE_DIR};${IV_THIRD_PARTY_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${_iv_core_runtime_link_libraries}"
        )
    endif()
endfunction()

function(iv_add_runtime_module target)
    set(options ENABLE_JUCE)
    set(oneValueArgs)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(IVM "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT IVM_SOURCES)
        message(FATAL_ERROR "iv_add_runtime_module(${target} ...) requires at least one source in SOURCES")
    endif()

    iv_configure_core_runtime_import()

    set(iv_runtime_module_user_sources ${IVM_SOURCES})
    set(iv_runtime_module_internal_sources "")
    if(NOT TARGET intravenous_module_runtime)
        iv_module_configure_juce()
        list(APPEND iv_runtime_module_internal_sources
            ${IV_CORE_INCLUDE_DIR}/basic_nodes/filters.cpp
            ${IV_CORE_INCLUDE_DIR}/basic_nodes/noise.cpp
            ${IV_CORE_INCLUDE_DIR}/basic_nodes/predictors.cpp
            ${IV_CORE_INCLUDE_DIR}/basic_nodes/shaping.cpp
        )
    endif()

    add_library(${target}__compile_settings INTERFACE)
    target_compile_features(${target}__compile_settings INTERFACE cxx_std_23)
    target_include_directories(${target}__compile_settings INTERFACE
        ${IV_CORE_INCLUDE_DIR}
        ${IV_MODULE_SOURCE_DIR}
    )
    target_include_directories(${target}__compile_settings SYSTEM INTERFACE
        ${IV_THIRD_PARTY_INCLUDE_DIR}
    )

    set(iv_runtime_module_source_dirs "")
    foreach(iv_runtime_module_source IN LISTS iv_runtime_module_user_sources)
        get_filename_component(iv_runtime_module_source_abs "${iv_runtime_module_source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_filename_component(iv_runtime_module_source_dir "${iv_runtime_module_source_abs}" DIRECTORY)
        list(APPEND iv_runtime_module_source_dirs "${iv_runtime_module_source_dir}")
    endforeach()
    list(REMOVE_DUPLICATES iv_runtime_module_source_dirs)
    if(iv_runtime_module_source_dirs)
        target_include_directories(${target}__compile_settings INTERFACE
            ${iv_runtime_module_source_dirs}
        )
    endif()

    if(MSVC)
        target_compile_options(${target}__compile_settings INTERFACE /W4 /permissive-)
    else()
        target_compile_options(${target}__compile_settings INTERFACE
            -Wall
            -Wextra
            -Wpedantic
        )
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target}__compile_settings INTERFACE
                -Wno-unused-comparison
            )
        endif()
    endif()

    if(IVM_ENABLE_JUCE)
        if(TARGET intravenous_module_runtime)
            if(DEFINED IV_CORE_ENABLE_JUCE_VST AND IV_CORE_ENABLE_JUCE_VST)
                target_compile_definitions(${target}__compile_settings INTERFACE
                    IV_ENABLE_JUCE_VST=1
                    JUCE_PLUGINHOST_VST3=1
                )
                if(DEFINED IV_JUCE_MODULES_DIR AND EXISTS "${IV_JUCE_MODULES_DIR}")
                    target_include_directories(${target}__compile_settings SYSTEM INTERFACE
                        ${IV_JUCE_MODULES_DIR}
                    )
                endif()
            else()
                target_compile_definitions(${target}__compile_settings INTERFACE
                    IV_ENABLE_JUCE_VST=0
                )
            endif()
        elseif(JUCE_FOUND)
            set(IV_JUCE_AUDIO_PROCESSORS_TARGET "")
            set(IV_JUCE_AUDIO_DEVICES_TARGET "")
            set(IV_JUCE_EVENTS_TARGET "")
            set(IV_JUCE_CORE_TARGET "")

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
                message(FATAL_ERROR "JUCE was requested but no supported JUCE CMake targets are available")
            endif()

            target_compile_definitions(${target}__compile_settings INTERFACE
                IV_ENABLE_JUCE_VST=1
                JUCE_PLUGINHOST_VST3=1
            )
            target_link_libraries(${target}__compile_settings INTERFACE
                ${IV_JUCE_AUDIO_PROCESSORS_TARGET}
                ${IV_JUCE_AUDIO_DEVICES_TARGET}
                ${IV_JUCE_EVENTS_TARGET}
                ${IV_JUCE_CORE_TARGET}
            )
        else()
            message(FATAL_ERROR "iv_add_runtime_module(${target} ENABLE_JUCE ...) requires JUCE, but JUCE was not found")
        endif()
    else()
        target_compile_definitions(${target}__compile_settings INTERFACE
            IV_ENABLE_JUCE_VST=0
        )
    endif()

    iv_rewrite_sources_to_build_dir(
        iv_runtime_module_user_sources
        TARGET ${target}
        COMPILE_SETTINGS_TARGET ${target}__compile_settings
        SOURCES ${iv_runtime_module_user_sources}
    )

    set(iv_runtime_module_build_sources ${iv_runtime_module_user_sources})
    list(APPEND iv_runtime_module_build_sources ${iv_runtime_module_internal_sources})

    add_library(${target} SHARED
        ${iv_runtime_module_build_sources}
    )

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        OUTPUT_NAME ${IV_MODULE_OUTPUT_NAME}
        RUNTIME_OUTPUT_DIRECTORY ${IV_MODULE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_DEBUG ${IV_MODULE_OUTPUT_DIR}
        RUNTIME_OUTPUT_DIRECTORY_RELEASE ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY_DEBUG ${IV_MODULE_OUTPUT_DIR}
        LIBRARY_OUTPUT_DIRECTORY_RELEASE ${IV_MODULE_OUTPUT_DIR}
    )

    target_compile_features(${target} PRIVATE cxx_std_23)
    target_link_libraries(${target} PRIVATE ${target}__compile_settings)

    target_precompile_headers(${target} PRIVATE ${IV_MODULE_PCH_HEADER})

    if(TARGET intravenous_module_runtime)
        target_link_libraries(${target} PRIVATE intravenous_module_runtime)
    endif()

    if(IVM_ENABLE_JUCE)
        if(TARGET intravenous_module_runtime)
            # Already handled through ${target}__compile_settings.
        elseif(JUCE_FOUND)
            iv_module_enable_juce(${target})
        endif()
    endif()
endfunction()
