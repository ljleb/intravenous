include(FetchContent)

set(_iv_default_llvm_tooling_version "22.1.3")
if(MSVC AND MSVC_VERSION VERSION_LESS 1944)
    set(_iv_default_llvm_tooling_version "19.1.1")
endif()

set(
    IV_LLVM_TOOLING_VERSION
    "${_iv_default_llvm_tooling_version}"
    CACHE STRING
    "Official LLVM/Clang release used for iv_clang_source_span_rewriter auto-fetch"
)

if(MSVC AND MSVC_VERSION VERSION_LESS 1944 AND IV_LLVM_TOOLING_VERSION STREQUAL "22.1.3")
    set(
        IV_LLVM_TOOLING_VERSION
        "19.1.1"
        CACHE STRING
        "Official LLVM/Clang release used for iv_clang_source_span_rewriter auto-fetch"
        FORCE
    )
endif()

find_package(LLVM CONFIG QUIET)
find_package(Clang CONFIG QUIET)

if(NOT LLVM_FOUND OR NOT Clang_FOUND)
    if(CMAKE_HOST_WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_iv_llvm_archive_name "clang+llvm-${IV_LLVM_TOOLING_VERSION}-x86_64-pc-windows-msvc.tar.xz")
        if(IV_LLVM_TOOLING_VERSION STREQUAL "19.1.1")
            set(_iv_llvm_archive_sha256 "621fc299fceb1bbdae927e355d1073034c9a1bbdda5a46a27e217c56af72f72a")
        elseif(IV_LLVM_TOOLING_VERSION STREQUAL "22.1.3")
            set(_iv_llvm_archive_sha256 "f2d7d200f72c83880aa2c1c00f6b6a67d0ea607ea663dafbd0cb2db6e2819ed0")
        else()
            unset(_iv_llvm_archive_sha256)
        endif()
    else()
        message(FATAL_ERROR
            "Automatic LLVM/Clang bootstrap for iv_clang_source_span_rewriter currently "
            "supports only 64-bit Windows MSVC hosts. "
            "Install LLVM/Clang manually and set LLVM_DIR / Clang_DIR or CMAKE_PREFIX_PATH."
        )
    endif()

    set(
        _iv_llvm_archive_url
        "https://github.com/llvm/llvm-project/releases/download/llvmorg-${IV_LLVM_TOOLING_VERSION}/${_iv_llvm_archive_name}"
    )
    string(REPLACE "." "_" _iv_llvm_version_key "${IV_LLVM_TOOLING_VERSION}")
    set(_iv_llvm_fetch_root "${CMAKE_BINARY_DIR}/_deps/iv_llvm_prebuilt-${_iv_llvm_version_key}")

    if(DEFINED _iv_llvm_archive_sha256)
        FetchContent_Declare(
            iv_llvm_prebuilt
            URL "${_iv_llvm_archive_url}"
            URL_HASH "SHA256=${_iv_llvm_archive_sha256}"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_DIR "${_iv_llvm_fetch_root}-src"
            BINARY_DIR "${_iv_llvm_fetch_root}-build"
            SUBBUILD_DIR "${_iv_llvm_fetch_root}-subbuild"
        )
    else()
        FetchContent_Declare(
            iv_llvm_prebuilt
            URL "${_iv_llvm_archive_url}"
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
            SOURCE_DIR "${_iv_llvm_fetch_root}-src"
            BINARY_DIR "${_iv_llvm_fetch_root}-build"
            SUBBUILD_DIR "${_iv_llvm_fetch_root}-subbuild"
        )
    endif()

    FetchContent_GetProperties(iv_llvm_prebuilt)
    if(NOT iv_llvm_prebuilt_POPULATED)
        message(STATUS "Downloading ${_iv_llvm_archive_name} for iv_clang_source_span_rewriter")
        FetchContent_Populate(iv_llvm_prebuilt)
    endif()

    set(_iv_llvm_prefix "${iv_llvm_prebuilt_SOURCE_DIR}")
    if(NOT EXISTS "${_iv_llvm_prefix}/lib/cmake/llvm/LLVMConfig.cmake")
        file(GLOB _iv_llvm_children LIST_DIRECTORIES true "${iv_llvm_prebuilt_SOURCE_DIR}/*")
        foreach(_iv_llvm_child IN LISTS _iv_llvm_children)
            if(EXISTS "${_iv_llvm_child}/lib/cmake/llvm/LLVMConfig.cmake")
                set(_iv_llvm_prefix "${_iv_llvm_child}")
                break()
            endif()
        endforeach()
    endif()

    if(NOT EXISTS "${_iv_llvm_prefix}/lib/cmake/llvm/LLVMConfig.cmake")
        message(FATAL_ERROR
            "Fetched LLVM archive did not contain ${_iv_llvm_prefix}/lib/cmake/llvm/LLVMConfig.cmake."
        )
    endif()

    if(NOT EXISTS "${_iv_llvm_prefix}/lib/cmake/clang/ClangConfig.cmake")
        message(FATAL_ERROR
            "Fetched LLVM archive did not contain ${_iv_llvm_prefix}/lib/cmake/clang/ClangConfig.cmake."
        )
    endif()

    list(PREPEND CMAKE_PREFIX_PATH "${_iv_llvm_prefix}")
    find_package(LLVM CONFIG REQUIRED)
    find_package(Clang CONFIG REQUIRED)
endif()

function(iv_fixup_llvm_diaguids_import)
    if(NOT WIN32 OR NOT TARGET LLVMDebugInfoPDB)
        return()
    endif()

    get_target_property(_iv_pdb_link_libs LLVMDebugInfoPDB INTERFACE_LINK_LIBRARIES)
    if(NOT _iv_pdb_link_libs)
        return()
    endif()

    set(_iv_needs_diaguids_fixup OFF)
    foreach(_iv_link_lib IN LISTS _iv_pdb_link_libs)
        if(_iv_link_lib MATCHES [[diaguids\.lib$]] AND NOT EXISTS "${_iv_link_lib}")
            set(_iv_needs_diaguids_fixup ON)
            break()
        endif()
    endforeach()

    if(NOT _iv_needs_diaguids_fixup)
        return()
    endif()

    file(GLOB _iv_diaguids_candidates
        "C:/Program Files/Microsoft Visual Studio/2022/*/DIA SDK/lib/amd64/diaguids.lib"
        "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*/lib/onecore/x64/diaguids.lib"
    )
    list(SORT _iv_diaguids_candidates)

    if(NOT _iv_diaguids_candidates)
        message(FATAL_ERROR
            "LLVM/Clang imported target LLVMDebugInfoPDB references diaguids.lib, but the "
            "configured Visual Studio DIA SDK path is missing and no replacement diaguids.lib "
            "was found under C:/Program Files/Microsoft Visual Studio/2022."
        )
    endif()

    list(GET _iv_diaguids_candidates 0 _iv_diaguids_lib)
    set(_iv_fixed_pdb_link_libs "")
    foreach(_iv_link_lib IN LISTS _iv_pdb_link_libs)
        if(_iv_link_lib MATCHES [[diaguids\.lib$]] AND NOT EXISTS "${_iv_link_lib}")
            list(APPEND _iv_fixed_pdb_link_libs "${_iv_diaguids_lib}")
        else()
            list(APPEND _iv_fixed_pdb_link_libs "${_iv_link_lib}")
        endif()
    endforeach()

    set_property(TARGET LLVMDebugInfoPDB PROPERTY INTERFACE_LINK_LIBRARIES "${_iv_fixed_pdb_link_libs}")
endfunction()

iv_fixup_llvm_diaguids_import()

add_executable(iv_clang_source_span_rewriter
    iv_clang_source_span_rewriter.cpp
)

set_target_properties(iv_clang_source_span_rewriter PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

set(_iv_llvm_compile_definitions "")
set(_iv_llvm_compile_options "")
foreach(_iv_llvm_definition IN LISTS LLVM_DEFINITIONS)
    if(_iv_llvm_definition MATCHES "^-D(.+)$")
        list(APPEND _iv_llvm_compile_definitions "${CMAKE_MATCH_1}")
    else()
        list(APPEND _iv_llvm_compile_options "${_iv_llvm_definition}")
    endif()
endforeach()

if(_iv_llvm_compile_definitions)
    target_compile_definitions(iv_clang_source_span_rewriter PRIVATE
        ${_iv_llvm_compile_definitions}
    )
endif()

if(_iv_llvm_compile_options)
    target_compile_options(iv_clang_source_span_rewriter PRIVATE
        ${_iv_llvm_compile_options}
    )
endif()

target_include_directories(iv_clang_source_span_rewriter PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

if(MSVC)
    target_compile_options(iv_clang_source_span_rewriter PRIVATE
        /W4
        /permissive-
    )
else()
    target_compile_options(iv_clang_source_span_rewriter PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

if(TARGET clang-cpp)
    target_link_libraries(iv_clang_source_span_rewriter PRIVATE clang-cpp)
else()
    target_link_libraries(iv_clang_source_span_rewriter PRIVATE
        clangAST
        clangASTMatchers
        clangBasic
        clangFrontend
        clangLex
        clangSerialization
        clangTooling
        clangToolingCore
    )
endif()

# Some distributions (including Nix toolchains) require explicit LLVM linkage
# when translation units directly reference llvm::* symbols.
if(TARGET LLVM)
    target_link_libraries(iv_clang_source_span_rewriter PRIVATE LLVM)
elseif(TARGET LLVMSupport)
    target_link_libraries(iv_clang_source_span_rewriter PRIVATE LLVMSupport)
elseif(LLVM_LIBRARIES)
    target_link_libraries(iv_clang_source_span_rewriter PRIVATE ${LLVM_LIBRARIES})
endif()
