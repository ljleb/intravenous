#define IV_INTERNAL_TRANSLATION_UNIT

#include "runtime/app.h"

#include <iostream>

int main(int argc, char** argv)
{
    try {
        return iv::run_intravenous_cli(argc, argv);
    } catch (std::exception const& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
