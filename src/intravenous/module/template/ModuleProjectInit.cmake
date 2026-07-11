include_guard(GLOBAL)

if(
    DEFINED IV_MODULE_SHARED_LIBRARY
    AND NOT IV_MODULE_SHARED_LIBRARY STREQUAL ""
    AND EXISTS "${IV_MODULE_SHARED_LIBRARY}"
)
    get_filename_component(_iv_module_shared_dir "${IV_MODULE_SHARED_LIBRARY}" DIRECTORY)
    if(_iv_module_shared_dir)
        list(APPEND CMAKE_BUILD_RPATH "${_iv_module_shared_dir}")
        list(REMOVE_DUPLICATES CMAKE_BUILD_RPATH)
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

    link_libraries(iv_module_shared)
endif()
