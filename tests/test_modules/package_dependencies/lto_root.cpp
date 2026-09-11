#include "dependency_api.h"

extern "C" float iv_test_lto_root_value()
{
    return iv_test_lto_leaf_value() + 4.0f;
}
