#include "modules/noisy_saw_project.h"
#include "modules/module.h"
#include "runtime/system.h"

#include <iostream>
#include <stacktrace>

using namespace iv;

[[noreturn]] void terminate_stacktrace()
{
    std::cerr << "\nstd::terminate called\n";

    if (auto ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (std::exception const& e) {
            std::cerr << std::stacktrace::current() << '\n';
            std::cerr << "uncaught exception: " << e.what() << '\n';
        } catch (...) {
            std::cerr << "uncaught non-std exception\n";
        }
    } else {
        std::cerr << "no active exception\n";
    }

    std::abort();
}

int main()
{
#ifndef NDEBUG
    std::set_terminate(terminate_stacktrace);
#endif

    std::cout << "Audio running. Press Enter to quit.\n";

    System system;
    GraphBuilder builder;
    ModuleContext context(builder, system);
    TypeErasedModule module(modules::noisy_saw_project);
    NodeProcessor processor(system.wrap_root(module.build(context)));

    for (size_t global_index = 0; !system.is_shutdown_requested(); ++global_index) {
        processor.tick({}, global_index);
    }

    return 0;
}
