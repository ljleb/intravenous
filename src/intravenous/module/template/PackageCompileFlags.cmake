include_guard(GLOBAL)

# Compile flags shared by every IV package translation unit, the DSL PCH those
# units include, and the clangd command written for package sources. A PCH
# must agree with its users on the language options these flags set.
#
# Code generation: Clang LLVM as it is handed to the optimizer at O2 (TBAA,
# lifetime markers, no optnone), without running the optimizer. That is
# IV_PACKAGE_COMPILE_FLAGS plus `-Xclang -disable-llvm-passes`, which CMake
# target options must spell "SHELL:-Xclang -disable-llvm-passes" because
# CMake de-duplicates repeated options. iv_package_finalize runs LLVM's
# pre-link simplification once over the linked package.
#
# Realtime: package code runs in the audio callback. The NixOS clang wrapper
# adds hardening and frame-pointer flags before these, so the flags below
# override them for package code, whatever the build environment enables:
# the stack protector, stack-clash probes, register zeroing on return, CET,
# pattern initialization of locals, -fwrapv, -fno-strict-aliasing (which would
# remove TBAA), libstdc++ assertions, and frame pointers. The wrapper defines
# _FORTIFY_SOURCE after these flags; module_pch.h undefines it instead.
set(IV_PACKAGE_COMPILE_FLAGS
    -O2
    -flto=full
    -fno-stack-protector
    -fno-stack-clash-protection
    -fzero-call-used-regs=skip
    -fcf-protection=none
    -ftrivial-auto-var-init=uninitialized
    -fno-wrapv
    -fstrict-aliasing
    -U_GLIBCXX_ASSERTIONS
    -fomit-frame-pointer
    -momit-leaf-frame-pointer)
