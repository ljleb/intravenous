include_guard(GLOBAL)

include(${IV_SOURCE_DIR}/module/template/JuceSupport.cmake)
include(${IV_SOURCE_DIR}/module/template/ModuleProjectInit.cmake)

option(IV_PACKAGE_SOURCE_INTROSPECTION
    "Collect configured IV module source identity metadata" ON)

set(IV_PACKAGE_FINALIZER_TIMINGS_FILE "" CACHE FILEPATH
    "Optional path for iv-package-finalize stage timings")

option(IV_PACKAGE_CLANG_TIME_TRACE
    "Write Clang frontend time-trace JSON for each IV package compilation" OFF)

function(iv_configure_builder_import)
    set(IV_BUILDER_LIBRARY "${IV_BUILDER_LIBRARY}" CACHE FILEPATH
        "Path to the built iv_builder library")
    if(NOT IV_BUILDER_LIBRARY OR NOT EXISTS "${IV_BUILDER_LIBRARY}")
        return()
    endif()
    if(NOT TARGET iv_builder)
        set(_iv_builder_links "")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            find_library(_iv_stdcxxexp_library NAMES stdc++exp
                HINTS ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES} REQUIRED)
            list(APPEND _iv_builder_links "${_iv_stdcxxexp_library}")
        endif()
        add_library(iv_builder SHARED IMPORTED GLOBAL)
        set_target_properties(iv_builder PROPERTIES
            IMPORTED_LOCATION "${IV_BUILDER_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${IV_INCLUDE_DIR};${IV_THIRD_PARTY_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${_iv_builder_links}")
    endif()
endfunction()

# Package compilation has three deliberately separate dependency classes:
#
# * LLVM inputs are linked by iv_package_finalize into the package bitcode.
# * native shared libraries are loaded into that package's ORC JITDylib.
# * interface/header usage requirements stay ordinary CMake requirements.
#
# These helpers are for dependencies that cannot be inferred from a CMake
# target.  In particular, do not pass a raw -lfoo item to either helper: name
# an imported CMake target or give an absolute library/object path instead.
function(_iv_package_require_target target caller)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "${caller}: '${target}' is not a CMake target")
    endif()
    get_property(_iv_package_target TARGET "${target}" PROPERTY IV_PACKAGE_OBJECT_TARGET)
    if(NOT _iv_package_target)
        message(FATAL_ERROR
            "${caller}: '${target}' was not created by iv_add_package()")
    endif()
endfunction()

function(_iv_package_append_llvm_input target item dependency)
    set_property(TARGET "${target}" APPEND PROPERTY IV_PACKAGE_LLVM_INPUTS "${item}")
    if(dependency)
        set_property(TARGET "${target}" APPEND PROPERTY IV_PACKAGE_DEPENDENCY_TARGETS
            "${dependency}")
    elseif(IS_ABSOLUTE "${item}")
        set_property(TARGET "${target}" APPEND PROPERTY IV_PACKAGE_FILE_DEPENDENCIES
            "${item}")
    endif()
endfunction()

function(_iv_package_append_dynamic_library target item dependency)
    set_property(TARGET "${target}" APPEND PROPERTY IV_PACKAGE_DYNAMIC_LIBRARIES "${item}")
    if(dependency)
        set_property(TARGET "${target}" APPEND PROPERTY IV_PACKAGE_DEPENDENCY_TARGETS
            "${dependency}")
    elseif(IS_ABSOLUTE "${item}")
        set_property(TARGET "${target}" APPEND PROPERTY IV_PACKAGE_FILE_DEPENDENCIES
            "${item}")
    endif()
endfunction()

function(iv_package_add_llvm_inputs target)
    _iv_package_require_target("${target}" "iv_package_add_llvm_inputs")
    foreach(_iv_item IN LISTS ARGN)
        if(TARGET "${_iv_item}")
            get_property(_iv_type TARGET "${_iv_item}" PROPERTY TYPE)
            get_property(_iv_imported TARGET "${_iv_item}" PROPERTY IMPORTED)
            if(_iv_imported)
                set(_iv_dependency "")
            else()
                set(_iv_dependency "${_iv_item}")
            endif()
            if(_iv_type STREQUAL "OBJECT_LIBRARY")
                _iv_package_append_llvm_input(
                    "${target}" "$<TARGET_OBJECTS:${_iv_item}>" "${_iv_dependency}")
                set_property(TARGET "${target}" APPEND PROPERTY IV_PACKAGE_FILE_DEPENDENCIES
                    "$<TARGET_OBJECTS:${_iv_item}>")
            elseif(_iv_type STREQUAL "STATIC_LIBRARY")
                _iv_package_append_llvm_input(
                    "${target}" "$<TARGET_LINKER_FILE:${_iv_item}>" "${_iv_dependency}")
                set_property(TARGET "${target}" APPEND PROPERTY IV_PACKAGE_FILE_DEPENDENCIES
                    "$<TARGET_LINKER_FILE:${_iv_item}>")
            else()
                message(FATAL_ERROR
                    "iv_package_add_llvm_inputs(${target}): target '${_iv_item}' is "
                    "${_iv_type}; use an OBJECT or STATIC library containing LLVM bitcode")
            endif()
        elseif(IS_ABSOLUTE "${_iv_item}")
            _iv_package_append_llvm_input("${target}" "${_iv_item}" "")
        else()
            message(FATAL_ERROR
                "iv_package_add_llvm_inputs(${target}): '${_iv_item}' must be a CMake "
                "OBJECT/STATIC target or an absolute LLVM object/archive path")
        endif()
    endforeach()
endfunction()

function(iv_package_add_dynamic_libraries target)
    _iv_package_require_target("${target}" "iv_package_add_dynamic_libraries")
    foreach(_iv_item IN LISTS ARGN)
        if(TARGET "${_iv_item}")
            get_property(_iv_type TARGET "${_iv_item}" PROPERTY TYPE)
            get_property(_iv_imported TARGET "${_iv_item}" PROPERTY IMPORTED)
            if(_iv_type STREQUAL "SHARED_LIBRARY" OR _iv_type STREQUAL "MODULE_LIBRARY"
               OR (_iv_type STREQUAL "UNKNOWN_LIBRARY" AND _iv_imported))
                if(_iv_imported)
                    set(_iv_dependency "")
                else()
                    set(_iv_dependency "${_iv_item}")
                endif()
                _iv_package_append_dynamic_library(
                    "${target}" "$<TARGET_FILE:${_iv_item}>" "${_iv_dependency}")
                set_property(TARGET "${target}" APPEND PROPERTY IV_PACKAGE_FILE_DEPENDENCIES
                    "$<TARGET_FILE:${_iv_item}>")
            else()
                message(FATAL_ERROR
                    "iv_package_add_dynamic_libraries(${target}): target '${_iv_item}' is "
                    "${_iv_type}; use a SHARED, MODULE, or imported UNKNOWN library")
            endif()
        elseif(IS_ABSOLUTE "${_iv_item}")
            _iv_package_append_dynamic_library("${target}" "${_iv_item}" "")
        else()
            message(FATAL_ERROR
                "iv_package_add_dynamic_libraries(${target}): '${_iv_item}' must be a "
                "shared-library target or an absolute native-library path")
        endif()
    endforeach()
endfunction()

function(_iv_package_collect_link_item target item)
    # CMake uses these directory-id wrappers when a target's link property
    # crosses directory boundaries. They are bookkeeping, not libraries.
    if(item MATCHES "^::@")
        return()
    endif()
    # PRIVATE static-library dependencies appear in the transitive interface
    # as LINK_ONLY. They are still LLVM inputs for package finalization.
    if(item MATCHES "^\\$<LINK_ONLY:(.*)>$")
        set(item "${CMAKE_MATCH_1}")
    endif()
    # The loader adds iv_builder explicitly to every package JITDylib. It is
    # the runtime boundary, never a package-local native dependency.
    if(item STREQUAL "iv_builder")
        return()
    endif()
    if(TARGET "${item}")
        get_property(_iv_visited TARGET "${target}" PROPERTY IV_PACKAGE_VISITED_LINK_TARGETS)
        list(FIND _iv_visited "${item}" _iv_seen_index)
        if(NOT _iv_seen_index EQUAL -1)
            return()
        endif()
        set_property(TARGET "${target}" APPEND PROPERTY IV_PACKAGE_VISITED_LINK_TARGETS "${item}")

        get_property(_iv_type TARGET "${item}" PROPERTY TYPE)
        get_property(_iv_imported TARGET "${item}" PROPERTY IMPORTED)
        if(_iv_type STREQUAL "OBJECT_LIBRARY" OR _iv_type STREQUAL "STATIC_LIBRARY")
            iv_package_add_llvm_inputs("${target}" "${item}")
        elseif(_iv_type STREQUAL "SHARED_LIBRARY" OR _iv_type STREQUAL "MODULE_LIBRARY"
               OR (_iv_type STREQUAL "UNKNOWN_LIBRARY" AND _iv_imported))
            iv_package_add_dynamic_libraries("${target}" "${item}")
        elseif(NOT _iv_type STREQUAL "INTERFACE_LIBRARY")
            message(FATAL_ERROR
                "iv_add_package(${target}): cannot classify linked target '${item}' "
                "(${_iv_type}); use iv_package_add_llvm_inputs() or "
                "iv_package_add_dynamic_libraries() explicitly")
        endif()

        # Preserve CMake's transitive link interface for static libraries and
        # interface targets. A shared library normally carries DT_NEEDED for
        # its own dependencies, but traversing its declared interface also
        # covers symbols used directly by package-owned LLVM.
        get_property(_iv_interface TARGET "${item}" PROPERTY INTERFACE_LINK_LIBRARIES)
        foreach(_iv_transitive_item IN LISTS _iv_interface)
            _iv_package_collect_link_item("${target}" "${_iv_transitive_item}")
        endforeach()
        return()
    endif()

    if(IS_ABSOLUTE "${item}")
        get_filename_component(_iv_extension "${item}" LAST_EXT)
        if(_iv_extension STREQUAL ".bc" OR _iv_extension STREQUAL ".o"
           OR _iv_extension STREQUAL ".obj" OR _iv_extension STREQUAL ".a"
           OR _iv_extension STREQUAL ".lib")
            iv_package_add_llvm_inputs("${target}" "${item}")
        elseif(_iv_extension STREQUAL ".so" OR _iv_extension STREQUAL ".dylib"
               OR _iv_extension STREQUAL ".dll")
            iv_package_add_dynamic_libraries("${target}" "${item}")
        else()
            message(FATAL_ERROR
                "iv_add_package(${target}): cannot classify absolute link item '${item}'; "
                "use iv_package_add_llvm_inputs() or "
                "iv_package_add_dynamic_libraries() explicitly")
        endif()
    elseif(item MATCHES "^\\$<")
        message(FATAL_ERROR
            "iv_add_package(${target}): cannot classify generator-expression link item "
            "'${item}'; use iv_package_add_llvm_inputs() or "
            "iv_package_add_dynamic_libraries() explicitly")
    else()
        # A bare item such as stdc++exp is a normal CMake usage requirement,
        # but no native package link exists to consume it. Leave it alone;
        # package-owned code requiring it must use one of the explicit APIs.
    endif()
endfunction()

function(_iv_package_collect_link_requirements target)
    set_property(TARGET "${target}" PROPERTY IV_PACKAGE_VISITED_LINK_TARGETS "")
    get_property(_iv_links TARGET "${target}" PROPERTY LINK_LIBRARIES)
    foreach(_iv_item IN LISTS _iv_links)
        _iv_package_collect_link_item("${target}" "${_iv_item}")
    endforeach()
endfunction()

function(_iv_package_define_finalizer target)
    get_property(_iv_finalizer_defined TARGET "${target}" PROPERTY IV_PACKAGE_FINALIZER_DEFINED)
    if(_iv_finalizer_defined)
        return()
    endif()
    set_property(TARGET "${target}" PROPERTY IV_PACKAGE_FINALIZER_DEFINED TRUE)

    _iv_package_collect_link_requirements("${target}")
    get_property(_iv_metadata_dir TARGET "${target}" PROPERTY IV_PACKAGE_METADATA_DIR)
    get_property(_iv_finalizer TARGET "${target}" PROPERTY IV_PACKAGE_FINALIZER)
    get_property(_iv_timings_file TARGET "${target}" PROPERTY IV_PACKAGE_FINALIZER_TIMINGS_FILE)
    get_property(_iv_output TARGET "${target}" PROPERTY IV_PACKAGE_OUTPUT)
    get_property(_iv_plugin TARGET "${target}" PROPERTY IV_PACKAGE_SOURCE_INTROSPECTION_PLUGIN)
    get_property(_iv_llvm_inputs TARGET "${target}" PROPERTY IV_PACKAGE_LLVM_INPUTS)
    get_property(_iv_dynamic_libraries TARGET "${target}" PROPERTY IV_PACKAGE_DYNAMIC_LIBRARIES)
    get_property(_iv_dependencies TARGET "${target}" PROPERTY IV_PACKAGE_DEPENDENCY_TARGETS)
    get_property(_iv_file_dependencies TARGET "${target}" PROPERTY IV_PACKAGE_FILE_DEPENDENCIES)

    list(REMOVE_DUPLICATES _iv_llvm_inputs)
    list(REMOVE_DUPLICATES _iv_dynamic_libraries)
    list(REMOVE_DUPLICATES _iv_dependencies)
    list(REMOVE_DUPLICATES _iv_file_dependencies)
    set(_iv_dynamic_manifest "${_iv_output}.dynamic-libraries")
    get_filename_component(_iv_output_dir "${_iv_output}" DIRECTORY)
    get_filename_component(_iv_output_name "${_iv_output}" NAME)
    file(MAKE_DIRECTORY "${_iv_output_dir}")
    if(_iv_dynamic_libraries)
        string(REPLACE ";" "\n" _iv_dynamic_manifest_content "${_iv_dynamic_libraries}")
        string(APPEND _iv_dynamic_manifest_content "\n")
    else()
        set(_iv_dynamic_manifest_content "")
    endif()
    file(GENERATE OUTPUT "${_iv_dynamic_manifest}" CONTENT "${_iv_dynamic_manifest_content}")

    add_custom_command(
        OUTPUT "${_iv_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_iv_output_dir}"
        COMMAND "${_iv_finalizer}"
            "--metadata-dir=${_iv_metadata_dir}"
            "--timings-file=${_iv_timings_file}"
            "--output=${_iv_output}"
            -- $<TARGET_OBJECTS:${target}> ${_iv_llvm_inputs}
        DEPENDS
            ${target}
            ${_iv_dependencies}
            ${_iv_file_dependencies}
            "${_iv_finalizer}"
            "${_iv_plugin}"
            "${_iv_dynamic_manifest}"
        COMMAND_EXPAND_LISTS
        VERBATIM
        COMMENT "Finalizing IV package LLVM ${_iv_output_name}")
    set(_iv_artifact_target "${target}__finalized_artifact")
    add_custom_target(${_iv_artifact_target} DEPENDS "${_iv_output}")
    add_dependencies(${target}__finalized ${_iv_artifact_target})
    if(_iv_dependencies)
        add_dependencies(${_iv_artifact_target} ${_iv_dependencies})
    endif()
endfunction()

function(iv_add_package target)
    set(options ENABLE_JUCE)
    set(oneValueArgs)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(IVP "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 23)
        message(FATAL_ERROR
            "IV packages require Clang 23 or newer; configured compiler is "
            "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})")
    endif()
    if(NOT DEFINED IV_PACKAGE_FINALIZER OR IV_PACKAGE_FINALIZER STREQUAL ""
       OR NOT EXISTS "${IV_PACKAGE_FINALIZER}")
        message(FATAL_ERROR "iv_add_package(${target}) requires IV_PACKAGE_FINALIZER")
    endif()
    if(NOT DEFINED IV_CLANG_SOURCE_INTROSPECTION_PLUGIN
       OR IV_CLANG_SOURCE_INTROSPECTION_PLUGIN STREQUAL ""
       OR NOT EXISTS "${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}")
        message(FATAL_ERROR
            "iv_add_package(${target}) requires IV_CLANG_SOURCE_INTROSPECTION_PLUGIN")
    endif()

    iv_configure_builder_import()

    set(_iv_metadata_dir "${CMAKE_CURRENT_BINARY_DIR}/iv-package-metadata")
    file(MAKE_DIRECTORY "${_iv_metadata_dir}")

    set(_iv_package_sources
        ${IV_PACKAGE_SOURCE_FILES}
        ${IVP_SOURCES})
    list(REMOVE_DUPLICATES _iv_package_sources)

    # IV packages are LLVM inputs, not native shared libraries.  Compile each
    # translation unit to full-LTO LLVM bitcode at O0, then combine/prune those
    # objects into one .ivpkg.bc consumed directly by the host's shared ORC JIT.
    # The public package target is the LLVM object target itself. Custom package
    # CMake can use ordinary target_compile_*/target_link_* commands on it; no
    # native package library is linked.
    set(_iv_objects_target "${target}")
    add_library(${_iv_objects_target} OBJECT ${_iv_package_sources})
    set_property(TARGET ${_iv_objects_target} PROPERTY IV_PACKAGE_OBJECT_TARGET TRUE)
    set_target_properties(${_iv_objects_target} PROPERTIES
        CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    target_compile_features(${_iv_objects_target} PRIVATE cxx_std_26)
    target_compile_options(${_iv_objects_target} PRIVATE -O0 -flto=full)
    target_compile_definitions(${_iv_objects_target} PRIVATE
        "IV_PACKAGE_ROOT=\"${IV_PACKAGE_DIR}\"")

    if(IV_PACKAGE_CLANG_TIME_TRACE)
        target_compile_options(${_iv_objects_target} PRIVATE -ftime-trace)
    endif()

    if(IV_PACKAGE_SOURCE_INTROSPECTION)
        set(_iv_source_introspection 1)
    else()
        set(_iv_source_introspection 0)
    endif()
    target_compile_options(${_iv_objects_target} PRIVATE
        "-fplugin=${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}"
        "-fplugin-arg-iv_module_metadata-core-source-dir=${IV_SOURCE_DIR}"
        "-fplugin-arg-iv_module_metadata-metadata-dir=${_iv_metadata_dir}"
        "-fplugin-arg-iv_module_metadata-source-introspection=${_iv_source_introspection}"
        -Wall -Wextra -Wpedantic)

    target_include_directories(${_iv_objects_target} PRIVATE
        ${IV_INCLUDE_DIR}
        ${IV_PACKAGE_DIR})
    if(DEFINED IV_PACKAGE_INCLUDE_DIRS AND NOT IV_PACKAGE_INCLUDE_DIRS STREQUAL "")
        target_include_directories(${_iv_objects_target} PRIVATE ${IV_PACKAGE_INCLUDE_DIRS})
    endif()
    target_include_directories(${_iv_objects_target} SYSTEM PRIVATE
        ${IV_THIRD_PARTY_INCLUDE_DIR})

    if(IVP_ENABLE_JUCE AND DEFINED IV_CORE_ENABLE_JUCE_VST AND IV_CORE_ENABLE_JUCE_VST)
        target_compile_definitions(${_iv_objects_target} PRIVATE
            IV_ENABLE_JUCE_VST=1 JUCE_PLUGINHOST_VST3=1)
        if(DEFINED IV_JUCE_MODULES_DIR AND EXISTS "${IV_JUCE_MODULES_DIR}")
            target_include_directories(${_iv_objects_target} SYSTEM PRIVATE
                ${IV_JUCE_MODULES_DIR})
        endif()
    else()
        target_compile_definitions(${_iv_objects_target} PRIVATE IV_ENABLE_JUCE_VST=0)
    endif()

    # Linking iv_builder as a usage requirement is useful for custom package
    # CMake projects (include/link properties), but no native link is performed.
    if(TARGET iv_builder)
        target_link_libraries(${_iv_objects_target} PRIVATE iv_builder)
    endif()

    if(NOT DEFINED IV_PACKAGE_PCH_HEADER)
        set(IV_PACKAGE_PCH_HEADER "${IV_SOURCE_DIR}/module/template/module_pch.h")
    endif()
    if(NOT IV_PACKAGE_PCH_HEADER STREQUAL "")
        target_precompile_headers(${_iv_objects_target} PRIVATE "${IV_PACKAGE_PCH_HEADER}")
    endif()

    set_property(SOURCE ${_iv_package_sources} APPEND PROPERTY OBJECT_DEPENDS
        "${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}")

    set(_iv_package_finalizer_timings_file "${IV_PACKAGE_FINALIZER_TIMINGS_FILE}")
    if(NOT _iv_package_finalizer_timings_file)
        set(_iv_package_finalizer_timings_file
            "${CMAKE_CURRENT_BINARY_DIR}/iv-package-finalizer-timings.txt")
    endif()

    set_property(TARGET ${_iv_objects_target} PROPERTY
        IV_PACKAGE_METADATA_DIR "${_iv_metadata_dir}")
    set_property(TARGET ${_iv_objects_target} PROPERTY
        IV_PACKAGE_FINALIZER "${IV_PACKAGE_FINALIZER}")
    set_property(TARGET ${_iv_objects_target} PROPERTY
        IV_PACKAGE_FINALIZER_TIMINGS_FILE "${_iv_package_finalizer_timings_file}")
    set_property(TARGET ${_iv_objects_target} PROPERTY
        IV_PACKAGE_OUTPUT "${IV_PACKAGE_OUTPUT_DIR}/${IV_PACKAGE_OUTPUT_NAME}.ivpkg.bc")
    set_property(TARGET ${_iv_objects_target} PROPERTY
        IV_PACKAGE_SOURCE_INTROSPECTION_PLUGIN "${IV_CLANG_SOURCE_INTROSPECTION_PLUGIN}")

    # This is a public package-CMake target. It must exist immediately so
    # custom CMake may add its own dependency edges after iv_add_package().
    # Its artifact-producing dependency is installed by the deferred step.
    add_custom_target(${target}__finalized ALL)

    # Custom package CMake conventionally adds target_link_libraries() after
    # iv_add_package(). Finalizer inputs must therefore be collected only once
    # that directory has finished evaluating.
    # DEFER evaluates arguments after this function's scope has unwound. Expand
    # the target now, then defer a bracket-quoted literal rather than an empty
    # out-of-scope ${target} variable.
    cmake_language(EVAL CODE
        "cmake_language(DEFER CALL _iv_package_define_finalizer [[${target}]])")
endfunction()
