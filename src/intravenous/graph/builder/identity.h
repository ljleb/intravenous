#pragma once

#include <string>

namespace iv {
    struct GraphBuilderIdentity {
        std::string value {};

        GraphBuilderIdentity() = default;
        explicit GraphBuilderIdentity(std::string value_);
        std::string child_id(size_t index) const;
    };
}
