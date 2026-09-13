include_guard(GLOBAL)

if(
    DEFINED IV_BUILDER_LIBRARY
    AND NOT IV_BUILDER_LIBRARY STREQUAL ""
    AND EXISTS "${IV_BUILDER_LIBRARY}"
)
    get_filename_component(_iv_builder_dir "${IV_BUILDER_LIBRARY}" DIRECTORY)
    if(_iv_builder_dir)
        list(APPEND CMAKE_BUILD_RPATH "${_iv_builder_dir}")
        list(REMOVE_DUPLICATES CMAKE_BUILD_RPATH)
    endif()

    if(NOT TARGET iv_builder)
        set(_iv_builder_link_libraries "")
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            find_library(_iv_stdcxxexp_library NAMES stdc++exp
                HINTS ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES} REQUIRED)
            list(APPEND _iv_builder_link_libraries "${_iv_stdcxxexp_library}")
        endif()

        add_library(iv_builder SHARED IMPORTED GLOBAL)
        set_target_properties(iv_builder PROPERTIES
            IMPORTED_LOCATION "${IV_BUILDER_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${IV_INCLUDE_DIR};${IV_THIRD_PARTY_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES "${_iv_builder_link_libraries}"
        )
    endif()

    link_libraries(iv_builder)
endif()
