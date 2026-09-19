#pragma once

// Compatibility include for source surfaces that still use the historical
// IV-module naming. The application module itself is NodeInstances.
#include <intravenous/runtime/node_instances.h>

namespace iv {
using IvModuleInstances = NodeInstances;
}
