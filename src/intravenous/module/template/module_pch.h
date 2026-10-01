#pragma once

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
