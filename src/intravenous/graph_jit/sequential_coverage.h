#pragma once

#include <intravenous/coverage.h>
#include <intravenous/compat.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace iv::graph_jit {

// A source output's exact coverage, shifted into a destination's value-lattice
// coordinates. No index means the compiler proves coverage of the whole circle.
// A present index without a published entry means unknown, not empty.
struct SequentialCoverageSource {
    std::optional<std::size_t> output_change{};
    std::size_t read_latency = 0;
    bool operator==(SequentialCoverageSource const&) const = default;
};

// Sources within a projection must all exist. Projections contribute
// independently, so their coverages are unioned at a Sequential input.
struct SequentialCoverageProjection {
    std::vector<SequentialCoverageSource> sources{};
    std::vector<std::size_t> target_channels{};
    bool operator==(SequentialCoverageProjection const&) const = default;
};

enum class SequentialCoveragePresence { absent, covered, unknown };

// Cold coverage algebra only; realtime point queries never construct sets.
[[nodiscard]] inline IndexCoverage const& full_index_coverage()
{
    constexpr auto half = std::uint64_t{1} << 63;
    static IndexCoverage const full{
        GlobalIndexRange{GlobalIndex::from_bits(0), half},
        GlobalIndexRange{GlobalIndex::from_bits(half), half}};
    return full;
}

[[nodiscard]] inline SequentialCoveragePresence projection_presence(
    std::span<SequentialCoverageSource const> sources,
    std::span<IndexCoverage const> output_coverages,
    GlobalIndex index) noexcept
{
    if (sources.empty()) return SequentialCoveragePresence::absent;
    bool unknown = false;
    for (auto const& source : sources) {
        if (!source.output_change) continue;
        if (*source.output_change >= output_coverages.size()) {
            unknown = true;
        } else if (!output_coverages[*source.output_change].contains(
                       index.retreated_by(source.read_latency))) {
            return SequentialCoveragePresence::absent;
        }
    }
    return unknown ? SequentialCoveragePresence::unknown
                   : SequentialCoveragePresence::covered;
}

[[nodiscard]] inline SequentialCoveragePresence input_presence(
    std::span<SequentialCoverageProjection const> projections,
    std::span<IndexCoverage const> output_coverages,
    GlobalIndex index,
    std::optional<std::size_t> channel = {}) noexcept
{
    bool unknown = false;
    for (auto const& projection : projections) {
        if (channel) {
            bool contributes = false;
            for (auto target : projection.target_channels)
                contributes |= target == *channel;
            if (!contributes) continue;
        }
        auto const presence = projection_presence(
            projection.sources, output_coverages, index);
        if (presence == SequentialCoveragePresence::covered) return presence;
        unknown |= presence == SequentialCoveragePresence::unknown;
    }
    return unknown ? SequentialCoveragePresence::unknown
                   : SequentialCoveragePresence::absent;
}

// Background-only set algebra. The same normalized source references feed
// the allocation-free point queries used by the realtime root above.
[[nodiscard]] inline IndexCoverage projection_coverage(
    std::span<SequentialCoverageSource const> sources,
    std::span<IndexCoverage const> output_coverages)
{
    if (sources.empty()) return {};
    constexpr auto half = std::uint64_t{1} << 63;
    IndexCoverage result{
        GlobalIndexRange{GlobalIndex::from_bits(0), half},
        GlobalIndexRange{GlobalIndex::from_bits(half), half}};
    for (auto const& source : sources) {
        if (!source.output_change) continue;
        // Background propagation supplies one complete selected candidate.
        // Unknown coverage must be resolved before this operation is called.
        IV_ASSERT(*source.output_change < output_coverages.size(),
            "Sequential source coverage is unresolved");
        IndexCoverage shifted;
        for (auto const range : output_coverages[*source.output_change]) {
            shifted.include({range.begin.advanced_by(source.read_latency), range.extent});
        }
        result = result & shifted;
    }
    return result;
}

} // namespace iv::graph_jit
