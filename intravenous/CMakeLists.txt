cmake_minimum_required(VERSION 3.20)

project(intravenous LANGUAGES C CXX)

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

add_library(intravenous_module_runtime STATIC
    module/loader.cpp
    module/watcher.cpp
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
)

add_executable(intravenous
    third_party/miniaudio/miniaudio.c
    main.cpp
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
)
