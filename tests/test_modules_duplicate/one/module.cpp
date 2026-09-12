#include <intravenous/dsl.h>

void duplicate_one(iv::GraphBuilder& builder)
{
    builder.outputs();
}

IV_MODULE("iv.test.duplicate", duplicate_one);
