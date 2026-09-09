#pragma once

#include <intravenous/dsl.h>

// This is the complete module-facing graph API. Parse it once for every
// module target so individual module sources only pay for their own code.
#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/debug_probe.h>
#include <intravenous/basic_nodes/filters.h>
#include <intravenous/basic_nodes/noise.h>
#include <intravenous/basic_nodes/polyphonic.h>
#include <intravenous/basic_nodes/predictors.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_nodes/shaping.h>
#include <intravenous/basic_nodes/timing.h>
#include <intravenous/basic_nodes/type_erased.h>
#include <intravenous/basic_nodes/weak_type_erased.h>
#include <intravenous/juce/vst_wrapper.h>
