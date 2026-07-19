include_guard(GLOBAL)

include(${IV_SOURCE_DIR}/module/template/JuceSupport.cmake)
include(${IV_SOURCE_DIR}/module/template/SourceSpanRewrite.cmake)
include(${IV_SOURCE_DIR}/module/template/ModuleProjectInit.cmake)

function(iv_configure_iv_module_shared_import)
    set(IV_MODULE_SHARED_LIBRARY "${IV_MODULE_SHARED_LIBRARY}" CACHE FILEPATH "Path to the built iv_module_shared library")

    if(NOT IV_MODULE_SHARED_LIBRARY OR NOT EXISTS "${IV_MODULE_SHARED_LIBRARY}")
        return()
    endif()

    if(NOT TARGET iv_module_shared)
        set(_iv_module_shared_link_libraries "")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            list(APPEND _iv_module_shared_link_libraries stdc++exp)
        endif()

        add_library(iv_module_shared SHARED IMPORTED GLOBAL)
        set_target_properties(iv_module_shared PROPERTIES
            IMPORTED_LOCATION "${IV_MODULE_SHARED_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${IV_INCLUDE_DIR};${IV_THIRD_PARTY_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${_iv_module_shared_link_libraries}"
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

    iv_configure_iv_module_shared_import()

    set(iv_runtime_module_user_sources ${IVM_SOURCES})

    add_library(${target}__compile_settings INTERFACE)
    target_compile_features(${target}__compile_settings INTERFACE cxx_std_23)
    target_include_directories(${target}__compile_settings INTERFACE
        ${IV_INCLUDE_DIR}
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
        if(TARGET iv_module_shared)
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

    add_library(${target} SHARED
        ${iv_runtime_module_user_sources}
    )

    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
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

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        if(IV_MODULE_ENABLE_TIME_TRACE OR IV_MODULE_PROFILE_COMPILATION)
            # JSON timeline for template parsing/instantiation, optimization,
            # and code generation. The profile mode below additionally emits
            # a textual aggregate report into build.trace.log.
            target_compile_options(${target} PRIVATE -ftime-trace)
        endif()
        if(IV_MODULE_PROFILE_COMPILATION)
            # Clang prints pass/category totals to stderr; ModuleLoader
            # captures it in the persistent module build trace.
            target_compile_options(${target} PRIVATE -ftime-report)
        endif()
    endif()

    if(IV_MODULE_PROFILE_BUILD_STEPS)
        # CMake prefixes every compile and link command with this launcher.
        # Its elapsed time is captured by ModuleLoader's persistent build log.
        set_property(TARGET ${target} PROPERTY RULE_LAUNCH_COMPILE "${CMAKE_COMMAND} -E time")
        set_property(TARGET ${target} PROPERTY RULE_LAUNCH_LINK "${CMAKE_COMMAND} -E time")
    endif()

    target_precompile_headers(${target} PRIVATE ${IV_MODULE_PCH_HEADER})

    if(TARGET iv_module_shared)
        target_link_libraries(${target} PRIVATE iv_module_shared)
    endif()

    if(IVM_ENABLE_JUCE)
        if(TARGET iv_module_shared)
            # Already handled through ${target}__compile_settings.
        elseif(JUCE_FOUND)
            iv_module_enable_juce(${target})
        endif()
    endif()
endfunction()
