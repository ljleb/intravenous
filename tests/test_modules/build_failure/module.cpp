#include <intravenous/dsl.h>

namespace {
void build_failure(iv::GraphBuilder& g)
{
    g.outputs();
}
}

this will not compile

IV_MODULE("iv.test.build_failure", build_failure);
