include_guard(GLOBAL)

if(NOT DEFINED IV_APPLICATION_ENVIRONMENT_DIRECTORY)
    set(IV_APPLICATION_ENVIRONMENT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
file(MAKE_DIRECTORY "${IV_APPLICATION_ENVIRONMENT_DIRECTORY}")
set(IV_APPLICATION_ENVIRONMENT_LAUNCHER "${IV_APPLICATION_ENVIRONMENT_DIRECTORY}/iv-toolchain-environment")
set(_iv_capture_identity "${IV_APPLICATION_ENVIRONMENT_DIRECTORY}/iv-toolchain-environment.compiler")
set(_iv_compiler_identity "${CMAKE_C_COMPILER}|${CMAKE_CXX_COMPILER}|${CMAKE_CXX_COMPILER_VERSION}")
option(IV_REFRESH_APPLICATION_ENVIRONMENT
    "Recapture this installation's compilation environment on this configure" OFF)
# Preserve the build's environment across automatic CMake regeneration launched
# outside the original development shell. A compiler change forces a recapture.
if(EXISTS "${IV_APPLICATION_ENVIRONMENT_LAUNCHER}" AND EXISTS "${_iv_capture_identity}"
   AND NOT IV_REFRESH_APPLICATION_ENVIRONMENT)
    file(READ "${_iv_capture_identity}" _iv_saved_compiler_identity)
    if(_iv_saved_compiler_identity STREQUAL _iv_compiler_identity)
        file(STRINGS "${IV_APPLICATION_ENVIRONMENT_LAUNCHER}" _iv_id_line
            REGEX "^export IV_APPLICATION_ENVIRONMENT_ID='")
        string(REGEX REPLACE "^export IV_APPLICATION_ENVIRONMENT_ID='([a-f0-9]+)'$"
            "\\1" IV_APPLICATION_ENVIRONMENT_ID "${_iv_id_line}")
        if(IV_APPLICATION_ENVIRONMENT_ID MATCHES "^[a-f0-9]+$")
            return()
        endif()
    endif()
endif()

# Capture compiler/build-tool inputs, never the user's complete environment.
# Keep the unset rules alongside the capture rules: an editor can inherit a
# different set of Nix wrapper variables from the shell that configured us.
set(_iv_environment_names PATH CC CXX CMAKE_PREFIX_PATH CMAKE_INCLUDE_PATH
    CMAKE_LIBRARY_PATH PKG_CONFIG_PATH PKG_CONFIG_LIBDIR PKG_CONFIG_SYSROOT_DIR
    CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH LIBRARY_PATH
    LD_LIBRARY_PATH GCC_EXEC_PREFIX COMPILER_PATH SDKROOT)
set(_iv_nix_environment_pattern
    "^NIX_(CC($|_.*)|BINTOOLS($|_.*)|CFLAGS_.*|CXXSTDLIB_.*|LDFLAGS.*|HARDENING_ENABLE.*|ENFORCE_.*|DYNAMIC_LINKER.*|DONT_SET_.*|STORE)$")
execute_process(COMMAND "${CMAKE_COMMAND}" -E environment
    OUTPUT_VARIABLE _iv_environment RESULT_VARIABLE _iv_environment_status)
if(NOT _iv_environment_status EQUAL 0)
    message(FATAL_ERROR "Cannot capture the application's compilation environment")
endif()
string(REGEX MATCHALL "(^|\n)[A-Za-z_][A-Za-z_0-9]*=" _iv_environment_entries "${_iv_environment}")
foreach(_iv_entry IN LISTS _iv_environment_entries)
    string(REGEX REPLACE "(^\n|=$)" "" _iv_name "${_iv_entry}")
    if(_iv_name MATCHES "${_iv_nix_environment_pattern}"
       AND NOT _iv_name MATCHES "^NIX_(CC|BINTOOLS)_WRAPPER_FLAGS_SET_")
        list(APPEND _iv_environment_names "${_iv_name}")
    endif()
endforeach()
list(REMOVE_DUPLICATES _iv_environment_names)
list(SORT _iv_environment_names)

function(iv_shell_quote out value)
    string(REPLACE "'" "'\"'\"'" escaped "${value}")
    set(${out} "'${escaped}'" PARENT_SCOPE)
endfunction()

function(iv_write_environment_artifact path content)
    if(EXISTS "${path}")
        file(READ "${path}" previous)
        if(previous STREQUAL content)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()

iv_shell_quote(_iv_cmake "${CMAKE_COMMAND}")
set(_iv_script "#!/bin/sh\nset -eu\n# Generated for this application build. No project environment is evaluated.\n")
# A pipeline is used only to list names. Unsets happen in the parent shell.
set(_iv_clear_environment [=[
iv_environment=$(@_iv_cmake@ -E environment)
for iv_name in $(printf '%s\n' "$iv_environment" | while IFS='=' read -r iv_name iv_value; do
  case "$iv_name" in
    NIX_CC|NIX_CC_*|NIX_BINTOOLS|NIX_BINTOOLS_*|NIX_CFLAGS_*|NIX_CXXSTDLIB_*|NIX_LDFLAGS*|NIX_HARDENING_ENABLE*|NIX_ENFORCE_*|NIX_DYNAMIC_LINKER*|NIX_DONT_SET_*|NIX_STORE|NIX_CC_WRAPPER_FLAGS_SET_*|NIX_BINTOOLS_WRAPPER_FLAGS_SET_*)
      printf '%s\n' "$iv_name" ;;
  esac
done); do unset "$iv_name"; done
]=])
string(CONFIGURE "${_iv_clear_environment}" _iv_clear_environment @ONLY)
string(APPEND _iv_script "${_iv_clear_environment}")
foreach(_iv_name IN LISTS _iv_environment_names)
    if(DEFINED ENV{${_iv_name}})
        iv_shell_quote(_iv_value "$ENV{${_iv_name}}")
        string(APPEND _iv_script "export ${_iv_name}=${_iv_value}\n")
    else()
        string(APPEND _iv_script "unset ${_iv_name}\n")
    endif()
endforeach()
string(SHA256 IV_APPLICATION_ENVIRONMENT_ID "${_iv_script}")
string(APPEND _iv_script "export IV_APPLICATION_ENVIRONMENT_ID='${IV_APPLICATION_ENVIRONMENT_ID}'\nexec \"$@\"\n")
iv_write_environment_artifact("${IV_APPLICATION_ENVIRONMENT_LAUNCHER}" "${_iv_script}")
iv_write_environment_artifact("${_iv_capture_identity}" "${_iv_compiler_identity}")
set(IV_REFRESH_APPLICATION_ENVIRONMENT OFF CACHE BOOL
    "Recapture this installation's compilation environment on this configure" FORCE)
file(CHMOD "${IV_APPLICATION_ENVIRONMENT_LAUNCHER}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# Other clients can select this local launcher without putting store paths in
# a project file. The external clangd extension continues to own its process.
find_program(IV_APPLICATION_CLANGD NAMES clangd HINTS "${LLVM_TOOLS_BINARY_DIR}")
if(IV_APPLICATION_CLANGD)
    iv_shell_quote(_iv_launcher "${IV_APPLICATION_ENVIRONMENT_LAUNCHER}")
    iv_shell_quote(_iv_clangd "${IV_APPLICATION_CLANGD}")
    iv_write_environment_artifact("${IV_APPLICATION_ENVIRONMENT_DIRECTORY}/intravenous-clangd"
        "#!/bin/sh\nexec ${_iv_launcher} ${_iv_clangd} \"$@\"\n")
    file(CHMOD "${IV_APPLICATION_ENVIRONMENT_DIRECTORY}/intravenous-clangd"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()
