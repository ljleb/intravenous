#include <intravenous/dsl.h>

namespace {
constexpr void build_failure(iv::GraphBuilder& g)
{
    g.outputs();
}
}

this will not compile
