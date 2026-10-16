#pragma once

#include <intravenous/continuous_value.h>
#include <intravenous/graph/port_ids.h>
#include <intravenous/node/port_constraints.h>

#include <cstddef>
#include <expected>
#include <optional>
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
    std::optional<std::size_t> size {};
    // Number of audio-basis samples represented by one value. Scalar-only
    // coordinate values have no audio sample support.
    std::optional<std::size_t> sample_support {};
    // Authored transported values per logical step of the owning node.
    PortPace local_pace {};
    // Transported values per canonical project sample after graph-wide pace
    // relations have been solved.
    PortPace absolute_pace {};
    std::size_t history = 0;
    std::size_t latency = 0;
    CoverageOrdering ordering = CoverageOrdering::unspecified;

    constexpr bool operator==(
        ResolvedContinuousPortRealization const&) const = default;
};

struct ResolvedEventPortRealization {
    std::size_t history = 0;
    std::size_t latency = 0;
    CoverageOrdering ordering = CoverageOrdering::unspecified;

    constexpr bool operator==(
        ResolvedEventPortRealization const&) const = default;
};

class ResolvedPortRealization {
    struct Bundle {
        std::vector<ResolvedContinuousPortRealization> inputs {};
        std::vector<ResolvedContinuousPortRealization> outputs {};
        std::vector<ResolvedEventPortRealization> event_inputs {};
        std::vector<ResolvedEventPortRealization> event_outputs {};
        PortPace execution_rate {};
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

    [[nodiscard]] ResolvedEventPortRealization const& event_input(
        NodeBundleHandle bundle,
        std::size_t port) const;

    [[nodiscard]] ResolvedEventPortRealization const& event_output(
        NodeBundleHandle bundle,
        std::size_t port) const;

    [[nodiscard]] PortPace execution_rate(NodeBundleHandle bundle) const;
};

[[nodiscard]] std::expected<ResolvedPortRealization, std::string>
resolve_port_realization(ConfiguredGraph const& graph);

} // namespace iv::graph_jit
