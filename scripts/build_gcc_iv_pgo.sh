#!/usr/bin/env bash
# Build GCC with feedback from Intravenous module compilation.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd -- "$script_dir/.." && pwd)

gcc_source="$repo_root/src/intravenous/third_party/gcc"
gcc_build="$repo_root/build/gcc-iv-pgo"
benchmark="$repo_root/build-release/benchmark/iv_module_build_benchmark"
module="$repo_root/projects/simple_sine/modules/saw"
workspace=""
profile_data=""
jobs=$(nproc)
gmp_include=""
gmp_lib=""
mpfr_include=""
mpfr_lib=""
mpc_prefix=""
native_system_header_dir=""
workspace_explicit=false
profile_data_explicit=false
resume_instrumented=false

usage() {
    cat <<'EOF'
Usage: scripts/build_gcc_iv_pgo.sh [options]

  --gcc-source PATH   GCC source checkout (default: src/intravenous/third_party/gcc)
  --build-dir PATH    Out-of-tree GCC build directory (default: build/gcc-iv-pgo)
  --benchmark PATH    Built iv_module_build_benchmark executable
  --module PATH       IV module directory or iv_module.json to train on
  --workspace PATH    Disposable benchmark workspace
  --profile-dir PATH  External GCC profile-data directory
  --jobs N            Parallel GCC build jobs
  --gmp-include PATH  Directory containing gmp.h
  --gmp-lib PATH      Directory containing libgmp
  --mpfr-include PATH Directory containing mpfr.h
  --mpfr-lib PATH     Directory containing libmpfr
  --mpc-prefix PATH   Prefix containing include/mpc.h and lib/libmpc
  --native-system-header-dir PATH
                       Directory containing the native libc headers
  --resume-instrumented
                       Reuse a completed instrumented GCC build, then train

The build directory must be empty. The resulting compiler is
<build-dir>/gcc/g++.
EOF
}

while (($#)); do
    case "$1" in
    --gcc-source) gcc_source=$2; shift 2 ;;
    --build-dir) gcc_build=$2; shift 2 ;;
    --benchmark) benchmark=$2; shift 2 ;;
    --module) module=$2; shift 2 ;;
    --workspace) workspace=$2; workspace_explicit=true; shift 2 ;;
    --profile-dir) profile_data=$2; profile_data_explicit=true; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    --gmp-include) gmp_include=$2; shift 2 ;;
    --gmp-lib) gmp_lib=$2; shift 2 ;;
    --mpfr-include) mpfr_include=$2; shift 2 ;;
    --mpfr-lib) mpfr_lib=$2; shift 2 ;;
    --mpc-prefix) mpc_prefix=$2; shift 2 ;;
    --native-system-header-dir) native_system_header_dir=$2; shift 2 ;;
    --resume-instrumented) resume_instrumented=true; shift ;;
    --help|-h) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if ! $workspace_explicit; then
    workspace="${gcc_build}-training"
fi
if ! $profile_data_explicit; then
    profile_data="${gcc_build}-profile-data"
fi

pkg_config_directory() {
    local flag=$1
    local package=$2
    local result
    result=$(pkg-config "$flag" "$package" 2>/dev/null || true)
    if [[ $result == -I* || $result == -L* ]]; then
        printf '%s\n' "${result:2}"
    fi
}

# Nix splits headers and libraries into different store paths.  GCC supports
# separate include/lib flags, so discover those from pkg-config instead of
# assuming a conventional prefix.  MPC is normally a single Nix output.
gmp_include=${gmp_include:-$(pkg_config_directory --cflags-only-I gmp)}
gmp_lib=${gmp_lib:-$(pkg_config_directory --libs-only-L gmp)}
mpfr_include=${mpfr_include:-$(pkg_config_directory --cflags-only-I mpfr)}
mpfr_lib=${mpfr_lib:-$(pkg_config_directory --libs-only-L mpfr)}

if [[ -z $mpc_prefix ]]; then
    for candidate in /nix/store/*-libmpc-*; do
        if [[ -f $candidate/include/mpc.h && -d $candidate/lib ]]; then
            mpc_prefix=$candidate
            break
        fi
    done
fi

# GCC's fixincludes defaults to /usr/include.  That directory is deliberately
# absent in Nix environments, where libc headers live in a glibc -dev output.
if [[ -z $native_system_header_dir ]]; then
    for candidate in /nix/store/*-glibc-*-dev/include; do
        if [[ -f $candidate/stdlib.h && -f $candidate/stdio.h ]]; then
            native_system_header_dir=$candidate
            break
        fi
    done
fi

for required in "$gmp_include/gmp.h" "$gmp_lib" \
                "$mpfr_include/mpfr.h" "$mpfr_lib" \
                "$mpc_prefix/include/mpc.h" "$mpc_prefix/lib" \
                "$native_system_header_dir/stdlib.h"; do
    if [[ -z $required || ! -e $required ]]; then
        echo "could not locate GCC prerequisite: $required" >&2
        echo "Pass --gmp-include/--gmp-lib, --mpfr-include/--mpfr-lib, and --mpc-prefix." >&2
        exit 2
    fi
done

for path in "$gcc_source" "$benchmark" "$module"; do
    if [[ ! -e $path ]]; then
        echo "required path does not exist: $path" >&2
        exit 2
    fi
done

# The benchmark owns its workspace and creates its marker before it ever
# clears it.  Pre-creating the directory here makes it look user-owned and
# causes the benchmark's safety check to reject it.
mkdir -p "$gcc_build" "$profile_data"

configure_gcc() {
    local flags=$1
    local link_flags=${LDFLAGS-}
    # Nix's GCC wrapper injects -Werror=format-security.  GCC's frontend
    # deliberately passes translated diagnostics through error/error_at, which
    # trips that distribution policy while building GCC itself.
    flags+=" -Wno-error=format-security"
    # Some GCC host tools are linked with LDFLAGS only. Their object files are
    # instrumented by CXXFLAGS, so link the profile-generation runtime too.
    if [[ $flags == *"-fprofile-generate="* ]]; then
        link_flags+=" -fprofile-generate=$profile_data"
    fi
    (
        cd "$gcc_build"
        CFLAGS="$flags" CXXFLAGS="$flags" LDFLAGS="$link_flags" "$gcc_source/configure" \
            --disable-bootstrap \
            --enable-languages=c,c++ \
            --disable-multilib \
            --with-gmp-include="$gmp_include" \
            --with-gmp-lib="$gmp_lib" \
            --with-mpfr-include="$mpfr_include" \
            --with-mpfr-lib="$mpfr_lib" \
            --with-mpc="$mpc_prefix" \
            --with-native-system-header-dir="$native_system_header_dir"
    )
}

create_training_compiler_wrappers() {
    local host_c=${CC:-gcc}
    local host_cxx=${CXX:-g++}
    local c_wrapper="$gcc_build/iv-pgo-cc"
    local cxx_wrapper="$gcc_build/iv-pgo-cxx"
    local -a cxx_include_dirs

    host_c=$(command -v "$host_c")
    host_cxx=$(command -v "$host_cxx")
    if [[ -z $host_c || -z $host_cxx ]]; then
        echo "could not locate host C/C++ compiler for GCC training" >&2
        exit 2
    fi

    # -B makes the normal Nix driver execute the newly built cc1plus. It also
    # changes the driver's standard-library prefix, so retain the include
    # search paths of the host C++ driver explicitly. The module workload is
    # C++, so leave CMake's C compiler on the normal host driver.
    mapfile -t cxx_include_dirs < <(
        "$host_cxx" -E -x c++ -v /dev/null 2>&1 \
            | sed -n '/#include <...> search starts here:/,/End of search list./ {
                s/^ //
                /End of search list/d
                /\/include/ p
            }')

    {
        printf '#!%s\n' "$BASH"
        printf 'exec %q "$@"\n' "$host_c"
    } > "$c_wrapper"
    {
        printf '#!%s\n' "$BASH"
        printf 'exec %q -B%q' "$host_cxx" "$gcc_build/gcc/"
        local include_dir
        for include_dir in "${cxx_include_dirs[@]}"; do
            printf ' -isystem %q' "$include_dir"
        done
        printf ' "$@"\n'
    } > "$cxx_wrapper"
    chmod +x "$c_wrapper" "$cxx_wrapper"
}

build_training_introspection_plugin() {
    local host_cxx=${CXX:-g++}
    local plugin_stage="$gcc_build/plugin-stage"
    local plugin_include
    local plugin_output="$gcc_build/iv_gcc_source_introspection_plugin.so"

    host_cxx=$(command -v "$host_cxx")
    if [[ -z $host_cxx ]]; then
        echo "could not locate host C++ compiler for local plugin build" >&2
        exit 2
    fi

    # GCC produces its complete plugin-header set through install-plugin, not
    # as an in-tree all-gcc output. Stage only those headers under the build
    # directory; this does not install or build target runtime libraries.
    make -C "$gcc_build/gcc" -j"$jobs" install-plugin DESTDIR="$plugin_stage"
    plugin_include=$(find "$plugin_stage" -type f -name gcc-plugin.h -printf '%h\n' -quit)
    if [[ -z $plugin_include || ! -f $plugin_include/plugin-version.h ]]; then
        echo "local GCC plugin headers are unavailable below: $plugin_stage" >&2
        exit 2
    fi

    "$host_cxx" -std=c++20 -fPIC -shared -fno-rtti \
        -Wall -Wextra -Wpedantic \
        -I"$plugin_include" \
        "$repo_root/tools/iv_gcc_source_introspection_plugin.cpp" \
        -o "$plugin_output"
}

if [[ -f $gcc_build/Makefile && $resume_instrumented == false ]]; then
    echo "GCC build directory must be empty: $gcc_build" >&2
    echo "Use --build-dir with a new directory, or remove this partial build." >&2
    exit 2
fi
if [[ $resume_instrumented == true && ! -x $gcc_build/gcc/cc1plus ]]; then
    echo "--resume-instrumented requires a completed all-gcc build: $gcc_build" >&2
    exit 2
fi

# GCC 16 forbids --enable-pgo-build for a native compiler. Perform the same
# two-pass process explicitly, with external profiles so distclean cannot
# discard the training data between passes.
if [[ $resume_instrumented == false ]]; then
    configure_gcc "-O3 -fprofile-generate=$profile_data"
    # We need the host compiler only. Building `all` also builds target
    # libgcc, whose native start-file assumptions do not hold in a Nix shell.
    make -C "$gcc_build" -j"$jobs" all-gcc
fi
create_training_compiler_wrappers
build_training_introspection_plugin

# The instrumented build itself invokes a few GCC programs. Those gcda files
# describe GCC's build, not the IV workload, so start collection cleanly.
find "$profile_data" -type f -name '*.gcda' -delete

"$benchmark" \
    --module "$module" \
    --workspace "$workspace" \
    --c-compiler "$gcc_build/iv-pgo-cc" \
    --cxx-compiler "$gcc_build/iv-pgo-cxx" \
    --gcc-source-introspection-plugin "$gcc_build/iv_gcc_source_introspection_plugin.so" \
    --gcc-time-report

make -C "$gcc_build" distclean
configure_gcc "-O3 -fprofile-use=$profile_data -fprofile-correction -Wno-missing-profile"
make -C "$gcc_build" -j"$jobs" all-gcc

echo "IV-trained PGO GCC: $gcc_build/gcc/g++"
