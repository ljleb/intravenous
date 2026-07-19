include_guard(GLOBAL)

function(iv_rewrite_sources_to_build_dir out_var)
    set(options)
    set(oneValueArgs TARGET COMPILE_SETTINGS_TARGET)
    set(multiValueArgs SOURCES)
    cmake_parse_arguments(IVSSR "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT IVSSR_TARGET)
        message(FATAL_ERROR "iv_rewrite_sources_to_build_dir(...) requires TARGET")
    endif()

    if(NOT IVSSR_SOURCES)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    set(_iv_compile_db_target "${IVSSR_TARGET}__source_span_compile_db")
    add_library(${_iv_compile_db_target} OBJECT ${IVSSR_SOURCES})
    set_target_properties(${_iv_compile_db_target} PROPERTIES
        EXCLUDE_FROM_ALL TRUE
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    if(IVSSR_COMPILE_SETTINGS_TARGET)
        target_link_libraries(${_iv_compile_db_target} PRIVATE ${IVSSR_COMPILE_SETTINGS_TARGET})
    endif()

    # These options are irrelevant to the database-only object itself, but
    # make its recorded command consistent with the consuming shared library.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(${_iv_compile_db_target} PRIVATE
            -fPIC
            -fvisibility=hidden
            -fvisibility-inlines-hidden
        )

    endif()

    # CMake drops implicit include directories from compile commands. Record
    # explicit flags for the database-only target because clangd does not query
    # Nix compiler wrappers for their system paths.
    if(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
        foreach(_iv_include_dir IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
            if(_iv_include_dir AND EXISTS "${_iv_include_dir}")
                target_compile_options(${_iv_compile_db_target} PRIVATE
                    "-isystem${_iv_include_dir}")
            endif()
        endforeach()
    endif()

    if(NOT DEFINED IV_SOURCE_SPAN_REWRITER OR IV_SOURCE_SPAN_REWRITER STREQUAL "")
        set(${out_var} ${IVSSR_SOURCES} PARENT_SCOPE)
        return()
    endif()

    if(NOT EXISTS "${IV_SOURCE_SPAN_REWRITER}")
        message(FATAL_ERROR
            "IV_SOURCE_SPAN_REWRITER points to '${IV_SOURCE_SPAN_REWRITER}', but that file does not exist"
        )
    endif()

    set(_iv_rewritten_sources "")
    set(_iv_rewriter_extra_args "")
    set(_iv_rewriter_root_args "")
    set(_iv_rewriter_pch_args "")
    set(_iv_rewriter_pch_dependency "")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_iv_rewriter_pch_dir "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${IVSSR_TARGET}.dir")
        set(_iv_rewriter_pch "${_iv_rewriter_pch_dir}/cmake_pch.hxx.pch")
        list(APPEND _iv_rewriter_pch_dependency "${_iv_rewriter_pch}")
    endif()
    if(DEFINED IV_INCLUDE_DIR AND NOT IV_INCLUDE_DIR STREQUAL "")
        get_filename_component(_iv_repo_root "${IV_INCLUDE_DIR}" DIRECTORY)
        if(EXISTS "${_iv_repo_root}")
            list(APPEND _iv_rewriter_root_args "--repo-root" "${_iv_repo_root}")
        endif()
    elseif(DEFINED IV_SOURCE_DIR AND NOT IV_SOURCE_DIR STREQUAL "")
        get_filename_component(_iv_repo_root "${IV_SOURCE_DIR}" DIRECTORY)
        if(EXISTS "${_iv_repo_root}")
            list(APPEND _iv_rewriter_root_args "--repo-root" "${_iv_repo_root}")
        endif()
    endif()
    if(CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
        foreach(_iv_include_dir IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
            if(_iv_include_dir AND EXISTS "${_iv_include_dir}")
                list(APPEND _iv_rewriter_extra_args "--extra-arg=-isystem${_iv_include_dir}")
            endif()
        endforeach()
    endif()

    foreach(_iv_source IN LISTS IVSSR_SOURCES)
        get_filename_component(_iv_source_abs "${_iv_source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

        if(DEFINED IV_MODULE_SOURCE_DIR AND NOT IV_MODULE_SOURCE_DIR STREQUAL "")
            file(RELATIVE_PATH _iv_source_rel "${IV_MODULE_SOURCE_DIR}" "${_iv_source_abs}")
        else()
            file(RELATIVE_PATH _iv_source_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_iv_source_abs}")
        endif()

        if(_iv_source_rel MATCHES "^\\.\\.")
            get_filename_component(_iv_source_rel "${_iv_source_abs}" NAME)
        endif()

        set(_iv_rewritten_source "${CMAKE_CURRENT_BINARY_DIR}/iv_source_spans/${_iv_source_rel}")
        get_filename_component(_iv_rewritten_dir "${_iv_rewritten_source}" DIRECTORY)

        add_custom_command(
            OUTPUT "${_iv_rewritten_source}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_iv_rewritten_dir}"
            COMMAND
                "${CMAKE_COMMAND}" -E time
                "${IV_SOURCE_SPAN_REWRITER}"
                -p "${CMAKE_BINARY_DIR}"
                ${_iv_rewriter_root_args}
                ${_iv_rewriter_extra_args}
                ${_iv_rewriter_pch_args}
                --effective-command-source "${_iv_rewritten_source}"
                --output "${_iv_rewritten_source}"
                "${_iv_source_abs}"
            DEPENDS
                "${IV_SOURCE_SPAN_REWRITER}"
                "${_iv_source_abs}"
                "${CMAKE_BINARY_DIR}/compile_commands.json"
                ${_iv_rewriter_pch_dependency}
            VERBATIM
        )

        set_source_files_properties("${_iv_rewritten_source}" PROPERTIES GENERATED TRUE)
        list(APPEND _iv_rewritten_sources "${_iv_rewritten_source}")
    endforeach()

    set(${out_var} ${_iv_rewritten_sources} PARENT_SCOPE)
endfunction()
