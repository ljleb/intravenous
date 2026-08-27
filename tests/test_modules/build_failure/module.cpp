#include <intravenous/dsl.h>

namespace {
consteval void build_failure(iv::GraphBuilder& g)
{
    g.outputs();
}
}

this will not compile
