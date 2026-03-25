include_guard(GLOBAL)

include(${IV_CORE_INCLUDE_DIR}/module/template/JuceSupport.cmake)

function(iv_configure_core_runtime_import)
    set(IV_CORE_RUNTIME_LIBRARY "${IV_CORE_RUNTIME_LIBRARY}" CACHE FILEPATH "Path to the built intravenous_module_runtime library")

    if(NOT IV_CORE_RUNTIME_LIBRARY OR NOT EXISTS "${IV_CORE_RUNTIME_LIBRARY}")
        return()
    endif()

    if(NOT TARGET intravenous_module_runtime)
        add_library(intravenous_module_runtime STATIC IMPORTED GLOBAL)
        set_target_properties(intravenous_module_runtime PROPERTIES
            IMPORTED_LOCATION "${IV_CORE_RUNTIME_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${IV_CORE_INCLUDE_DIR};${IV_THIRD_PARTY_INCLUDE_DIR}"
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

    set(iv_runtime_module_sources ${IVM_SOURCES})
    if(NOT TARGET intravenous_module_runtime)
        iv_module_configure_juce()
        list(APPEND iv_runtime_module_sources
            ${IV_CORE_INCLUDE_DIR}/basic_nodes/filters.cpp
            ${IV_CORE_INCLUDE_DIR}/basic_nodes/noise.cpp
            ${IV_CORE_INCLUDE_DIR}/basic_nodes/predictors.cpp
            ${IV_CORE_INCLUDE_DIR}/basic_nodes/shaping.cpp
        )
    endif()

    add_library(${target} SHARED
        ${iv_runtime_module_sources}
    )

    target_compile_features(${target} PRIVATE cxx_std_23)
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

    target_include_directories(${target} PRIVATE
        ${IV_CORE_INCLUDE_DIR}
        ${IV_MODULE_SOURCE_DIR}
    )
    target_include_directories(${target} SYSTEM PRIVATE
        ${IV_THIRD_PARTY_INCLUDE_DIR}
    )

    target_precompile_headers(${target} PRIVATE ${IV_MODULE_PCH_HEADER})

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(TARGET intravenous_module_runtime)
        target_link_libraries(${target} PRIVATE intravenous_module_runtime)
    endif()

    if(IVM_ENABLE_JUCE)
        if(TARGET intravenous_module_runtime)
            target_compile_definitions(${target} PRIVATE
                IV_ENABLE_JUCE_VST=1
                JUCE_PLUGINHOST_VST3=1
            )
            if(DEFINED IV_JUCE_MODULES_DIR AND EXISTS "${IV_JUCE_MODULES_DIR}")
                target_include_directories(${target} SYSTEM PRIVATE
                    ${IV_JUCE_MODULES_DIR}
                )
            endif()
        elseif(JUCE_FOUND)
            iv_module_enable_juce(${target})
        else()
            message(FATAL_ERROR "iv_add_runtime_module(${target} ENABLE_JUCE ...) requires JUCE, but JUCE was not found")
        endif()
    else()
        target_compile_definitions(${target} PRIVATE
            IV_ENABLE_JUCE_VST=0
        )
    endif()
endfunction()
