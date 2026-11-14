#pragma once

// Package code runs in the realtime audio callback. The NixOS clang wrapper
// defines _FORTIFY_SOURCE after every other compile flag, so
// PackageCompileFlags.cmake cannot override it. Undefining it here, before any
// system header is included, keeps glibc's checked memory and string functions
// out of package code.
#undef _FORTIFY_SOURCE

#include <intravenous/dsl.h>

// This is the complete module-facing graph API. The application builds this
// DSL PCH once; every IV package consumes that one artifact so package sources
// only pay to parse their own code.
#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/debug_probe.h>
#include <intravenous/basic_nodes/filters.h>
#include <intravenous/basic_nodes/noise.h>
#include <intravenous/basic_nodes/polyphonic.h>
#include <intravenous/basic_nodes/predictors.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/basic_nodes/timing.h>
#include <intravenous/juce/vst_wrapper.h>
