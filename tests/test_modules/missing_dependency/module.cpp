#include <intravenous/dsl.h>

namespace iv {
constexpr missing_dependency(GraphBuilder& g)
{
    g.outputs();
}
}

IV_MODULE("iv.test.missing_dependency", iv::missing_dependency);
