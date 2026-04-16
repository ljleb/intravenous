include_guard(GLOBAL)

function(iv_module_configure_juce)
    set(JUCE_DIR "${JUCE_DIR}" CACHE PATH "Path to a JUCE source tree or installed JUCE package root")

    set(IV_JUCE_SOURCE_DIR "")
    if(JUCE_DIR)
        if(EXISTS "${JUCE_DIR}/CMakeLists.txt")
            set(IV_JUCE_SOURCE_DIR "${JUCE_DIR}")
        elseif(EXISTS "${JUCE_DIR}/JUCEConfig.cmake")
            list(PREPEND CMAKE_PREFIX_PATH "${JUCE_DIR}")
        elseif(EXISTS "${JUCE_DIR}/lib/cmake/JUCE/JUCEConfig.cmake")
            list(PREPEND CMAKE_PREFIX_PATH "${JUCE_DIR}/lib/cmake/JUCE")
        endif()
    endif()

    if(IV_JUCE_SOURCE_DIR)
        add_subdirectory("${IV_JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/juce" EXCLUDE_FROM_ALL)
        set(JUCE_FOUND TRUE PARENT_SCOPE)
        return()
    endif()

    find_package(JUCE CONFIG QUIET)
    if(TARGET juce::juce_audio_processors OR TARGET JUCE::juce_audio_processors)
        set(JUCE_FOUND TRUE PARENT_SCOPE)
    endif()
endfunction()

function(iv_module_enable_juce target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "iv_module_enable_juce target '${target}' does not exist")
    endif()

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

    target_sources("${target}" PRIVATE
        ${IV_CORE_INCLUDE_DIR}/juce/vst_runtime.cpp
    )
    target_compile_definitions("${target}" PRIVATE
        IV_ENABLE_JUCE_VST=1
        JUCE_PLUGINHOST_VST3=1
    )
    target_link_libraries("${target}" PRIVATE
        ${IV_JUCE_AUDIO_PROCESSORS_TARGET}
        ${IV_JUCE_AUDIO_DEVICES_TARGET}
        ${IV_JUCE_EVENTS_TARGET}
        ${IV_JUCE_CORE_TARGET}
    )
endfunction()
