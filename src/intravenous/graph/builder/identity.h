#pragma once

#include <limits>
#include <string>

namespace iv {
    struct GraphBuilderIdentity {
        std::string value {};

        GraphBuilderIdentity() = default;
        constexpr explicit GraphBuilderIdentity(std::string value_)
        : value(std::move(value_))
        {}

        constexpr std::string child_id(size_t index) const
        {
            auto result = value;
            if (!result.empty()) {
                result += '.';
            }
            char digits[std::numeric_limits<size_t>::digits10 + 2] {};
            size_t begin = sizeof(digits);
            do {
                digits[--begin] = static_cast<char>('0' + index % 10);
                index /= 10;
            } while (index != 0);
            result.append(digits + begin, digits + sizeof(digits));
            return result;
        }
    };
}
