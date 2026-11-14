#define IV_INTERNAL_TRANSLATION_UNIT

#include <intravenous/runtime/app.h>

#include <iostream>
#include <cstdlib>
#include <cerrno>
#include <string_view>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <unistd.h>

int main(int argc, char** argv)
{
    try {
        // Re-exec once before creating application threads. The launcher
        // restores only compilation inputs and preserves the RPC socket.
        auto const* entered = std::getenv("IV_APPLICATION_ENVIRONMENT_ID");
        if (!entered || std::string_view(entered) != IV_CONFIGURED_APPLICATION_ENVIRONMENT_ID) {
            auto executable = std::filesystem::read_symlink("/proc/self/exe").string();
            std::vector<char*> arguments;
            arguments.push_back(const_cast<char*>(IV_CONFIGURED_APPLICATION_ENVIRONMENT_LAUNCHER));
            arguments.push_back(executable.data());
            for (int i = 1; i < argc; ++i) arguments.push_back(argv[i]);
            arguments.push_back(nullptr);
            ::execv(arguments.front(), arguments.data());
            throw std::system_error(errno, std::generic_category(),
                "cannot activate this application's compilation environment");
        }
        ::unsetenv("IV_APPLICATION_ENVIRONMENT_ID");
        return iv::run_intravenous_cli(argc, argv);
    } catch (std::exception const& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
