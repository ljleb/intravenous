#pragma once

// This is the module-facing node API. Keep it available through dsl.h:
// module authors need these port descriptions, traits, and execution contexts
// to define their own node types. It intentionally does not expose graph
// builder storage or GraphBuilder implementation details.
#include <intravenous/node/lifecycle.h>
#include <intravenous/node/traits.h>
#include <intravenous/ports.h>
