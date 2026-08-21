include_guard(GLOBAL)

# Rewrite exactly one IV graph-authoring entry into its generated extensionless
# definition-bearing import. Included headers are parsed for context but are
# never rewrite targets. Module-to-module ordering is passed explicitly through
# DEPENDS by the manifest/import resolver.
function(iv_rewrite_module_entry)
    set(options GLOBAL_MODULE)
    set(oneValueArgs TARGET SOURCE OUTPUT COMPILE_SETTINGS_TARGET PCH_HEADER)
    set(multiValueArgs DEPENDS)
    cmake_parse_arguments(IVR "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT IVR_TARGET OR NOT IVR_SOURCE OR NOT IVR_OUTPUT)
        message(FATAL_ERROR "iv_rewrite_module_entry requires TARGET, SOURCE, and OUTPUT")
    endif()

    get_filename_component(_iv_source_abs "${IVR_SOURCE}" ABSOLUTE)
    get_filename_component(_iv_source_dir "${_iv_source_abs}" DIRECTORY)
    get_filename_component(_iv_output_abs "${IVR_OUTPUT}" ABSOLUTE)
    get_filename_component(_iv_output_dir "${_iv_output_abs}" DIRECTORY)

    set(_iv_compile_db_target "${IVR_TARGET}__source_span_compile_db")
    add_library(${_iv_compile_db_target} OBJECT EXCLUDE_FROM_ALL "${_iv_source_abs}")
    set_target_properties(${_iv_compile_db_target} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)

    if(IVR_GLOBAL_MODULE)
        # A reusable global definition must never parse through the project
        # generated include tree. Its normal IV imports therefore resolve only
        # against the shared global tree, exactly as the canonicalized output
        # will later do.
        target_compile_features(${_iv_compile_db_target} PRIVATE cxx_std_23)
        target_include_directories(${_iv_compile_db_target} PRIVATE
            "${IV_INCLUDE_DIR}"
            "${_iv_source_dir}")
        if(DEFINED IV_GLOBAL_MODULE_GENERATED_INCLUDE_DIR AND
           NOT IV_GLOBAL_MODULE_GENERATED_INCLUDE_DIR STREQUAL "")
            target_include_directories(${_iv_compile_db_target} PRIVATE
                "${IV_GLOBAL_MODULE_GENERATED_INCLUDE_DIR}")
        endif()
        if(DEFINED IV_THIRD_PARTY_INCLUDE_DIR AND
           NOT IV_THIRD_PARTY_INCLUDE_DIR STREQUAL "")
            target_include_directories(${_iv_compile_db_target} SYSTEM PRIVATE
                "${IV_THIRD_PARTY_INCLUDE_DIR}")
        endif()
    elseif(IVR_COMPILE_SETTINGS_TARGET)
        target_link_libraries(${_iv_compile_db_target} PRIVATE ${IVR_COMPILE_SETTINGS_TARGET})
    endif()

    if(IVR_PCH_HEADER AND NOT IVR_GLOBAL_MODULE)
        target_precompile_headers(${_iv_compile_db_target} PRIVATE "${IVR_PCH_HEADER}")
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${_iv_compile_db_target} PRIVATE
            -fPIC -fvisibility=hidden -fvisibility-inlines-hidden)
    endif()
    foreach(_iv_include_dir IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
        if(_iv_include_dir AND EXISTS "${_iv_include_dir}")
            target_compile_options(${_iv_compile_db_target} PRIVATE "-isystem${_iv_include_dir}")
        endif()
    endforeach()

    if(NOT DEFINED IV_SOURCE_SPAN_REWRITER OR IV_SOURCE_SPAN_REWRITER STREQUAL "")
        add_custom_command(
            OUTPUT "${_iv_output_abs}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_iv_output_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_iv_source_abs}" "${_iv_output_abs}"
            DEPENDS "${_iv_source_abs}" ${IVR_DEPENDS}
            VERBATIM)
    else()
        if(NOT EXISTS "${IV_SOURCE_SPAN_REWRITER}")
            message(FATAL_ERROR "IV_SOURCE_SPAN_REWRITER does not exist: ${IV_SOURCE_SPAN_REWRITER}")
        endif()

        set(_iv_rewriter_args "")
        if(DEFINED IV_INCLUDE_DIR AND NOT IV_INCLUDE_DIR STREQUAL "")
            get_filename_component(_iv_repo_root "${IV_INCLUDE_DIR}" DIRECTORY)
            list(APPEND _iv_rewriter_args --repo-root "${_iv_repo_root}")
        endif()
        foreach(_iv_include_dir IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
            if(_iv_include_dir AND EXISTS "${_iv_include_dir}")
                list(APPEND _iv_rewriter_args "--extra-arg=-isystem${_iv_include_dir}")
            endif()
        endforeach()

        set(_iv_pch_dependency "")
        if(IVR_PCH_HEADER AND NOT IVR_GLOBAL_MODULE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(_iv_pch_dependency
                "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${_iv_compile_db_target}.dir/cmake_pch.hxx.pch")
        endif()

        set(_iv_global_postprocess "")
        if(IVR_GLOBAL_MODULE)
            list(APPEND _iv_global_postprocess
                COMMAND "${CMAKE_COMMAND}"
                    -DINPUT="${_iv_output_abs}"
                    -P "${IV_SOURCE_DIR}/module/template/CanonicalizeGlobalImports.cmake")
        endif()

        add_custom_command(
            OUTPUT "${_iv_output_abs}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_iv_output_dir}"
            COMMAND "${CMAKE_COMMAND}" -E time
                "${IV_SOURCE_SPAN_REWRITER}"
                -p "${CMAKE_BINARY_DIR}"
                ${_iv_rewriter_args}
                --effective-command-source "${_iv_source_abs}"
                --output "${_iv_output_abs}"
                "${_iv_source_abs}"
            ${_iv_global_postprocess}
            DEPENDS
                "${_iv_source_abs}"
                "${CMAKE_BINARY_DIR}/compile_commands.json"
                "${IV_SOURCE_SPAN_REWRITER}"
                ${_iv_pch_dependency}
                ${IVR_DEPENDS}
            VERBATIM)
    endif()

    set_source_files_properties("${_iv_output_abs}" PROPERTIES GENERATED TRUE)
endfunction()

# Compatibility helper for non-module rewrite users.
function(iv_rewrite_sources_to_build_dir out_var)
    set(options)
    set(oneValueArgs TARGET COMPILE_SETTINGS_TARGET PCH_HEADER)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(IVSSR "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
    set(_iv_outputs "")
    foreach(_iv_source IN LISTS IVSSR_SOURCES)
        get_filename_component(_iv_source_abs "${_iv_source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_filename_component(_iv_name "${_iv_source_abs}" NAME)
        set(_iv_output "${CMAKE_CURRENT_BINARY_DIR}/iv_source_spans/${_iv_name}")
        string(MAKE_C_IDENTIFIER "${IVSSR_TARGET}_${_iv_name}" _iv_target)
        iv_rewrite_module_entry(
            TARGET "${_iv_target}"
            SOURCE "${_iv_source_abs}"
            OUTPUT "${_iv_output}"
            COMPILE_SETTINGS_TARGET "${IVSSR_COMPILE_SETTINGS_TARGET}"
            PCH_HEADER "${IVSSR_PCH_HEADER}")
        list(APPEND _iv_outputs "${_iv_output}")
    endforeach()
    set(${out_var} ${_iv_outputs} PARENT_SCOPE)
endfunction()
