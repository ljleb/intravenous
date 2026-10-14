#pragma once

#include <intravenous/continuous_value.h>
#include <intravenous/graph/port_ids.h>

#include <cstddef>
#include <expected>
#include <string>
#include <utility>
#include <vector>

namespace iv {
struct ConfiguredGraph;
}

namespace iv::graph_jit {

namespace detail {
class PortRealizationSolver;
}

struct ResolvedContinuousPortRealization {
    ContinuousValueTypeId value_type = continuous_value_type_id_v<Sample>;

    constexpr bool operator==(
        ResolvedContinuousPortRealization const&) const = default;
};

class ResolvedPortRealization {
    struct Bundle {
        std::vector<ResolvedContinuousPortRealization> inputs {};
        std::vector<ResolvedContinuousPortRealization> outputs {};
    };

    std::vector<Bundle> bundles_ {};

    explicit ResolvedPortRealization(std::vector<Bundle> bundles)
        : bundles_(std::move(bundles))
    {}

    friend std::expected<ResolvedPortRealization, std::string>
        resolve_port_realization(ConfiguredGraph const&);
    friend class detail::PortRealizationSolver;

public:
    ResolvedPortRealization() = default;

    [[nodiscard]] std::size_t bundle_count() const noexcept
    {
        return bundles_.size();
    }

    [[nodiscard]] ResolvedContinuousPortRealization const& input(
        NodeBundleHandle bundle,
        std::size_t port) const;

    [[nodiscard]] ResolvedContinuousPortRealization const& output(
        NodeBundleHandle bundle,
        std::size_t port) const;
};

[[nodiscard]] std::expected<ResolvedPortRealization, std::string>
resolve_port_realization(ConfiguredGraph const& graph);

} // namespace iv::graph_jit
