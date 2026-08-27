#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace iv::details {
    [[noreturn]] constexpr void error(std::string message)
    {
        throw std::logic_error(std::move(message));
    }
}
