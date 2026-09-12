#include <intravenous/dsl.h>

namespace iv {
void missing_dependency(GraphBuilder& g)
{
    g.outputs(g.node<"iv.test.this_does_not_exist">());
}
}

IV_MODULE("iv.test.missing_dependency", iv::missing_dependency);
