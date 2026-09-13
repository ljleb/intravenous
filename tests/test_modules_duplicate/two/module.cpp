#include <intravenous/dsl.h>

void duplicate_two(iv::GraphBuilder& builder)
{
    builder.outputs();
}

IV_MODULE("iv.test.duplicate", duplicate_two);
