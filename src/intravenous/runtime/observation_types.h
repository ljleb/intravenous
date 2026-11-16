#pragma once

#include <intravenous/observation.h>
#include <intravenous/graph/port_ids.h>
#include <intravenous/runtime/project_connection_types.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iv {

inline constexpr std::size_t maximum_live_observation_probes = 256;

enum class ObservationSourceKind : std::uint8_t { output, state_field };
struct ObservationSource {
    NodeAddress node{};
    ObservationSourceKind kind = ObservationSourceKind::output;
    std::string member{};
    std::optional<std::size_t> channel{};
    bool operator==(ObservationSource const&) const = default;
};
struct ObservationRequirement {
    std::uint64_t id = 0;
    ObservationSource source{};
    std::string operation{};
    bool operator==(ObservationRequirement const&) const = default;
};
struct ObservationRequirements {
    std::uint64_t revision = 0;
    std::vector<ObservationRequirement> probes{};
    bool operator==(ObservationRequirements const&) const = default;
};

// ProjectGraph resolves semantic addresses in the same immutable configured
// graph used by this compilation. State fields require exactly one Tick node.
struct ResolvedObservationProbe {
    std::uint64_t id = 0;
    ObservationOperation const* operation = nullptr;
    ObservationSourceKind kind = ObservationSourceKind::output;
    NodeBundleHandle bundle = 0;
    std::size_t port = 0;
    std::vector<std::size_t> channels{};
    std::size_t state_field_offset = 0;
    std::size_t state_field_size = 0;
    bool available = false;
};
struct CompiledObservationPlan {
    std::uint64_t revision = 0;
    std::vector<ResolvedObservationProbe> probes{};
};

enum class ObservationValueStatus : std::uint8_t { unavailable, available };
struct RawObservationValue {
    std::uint64_t request_id = 0;
    ObservationOperation const* operation = nullptr;
    ObservationValueStatus status = ObservationValueStatus::unavailable;
    ObservationCaptureState value{};
};
struct LiveObservationFrame {
    std::uint64_t activation_id = 0;
    std::uint64_t project_generation = 0;
    std::uint64_t configured_generation = 0;
    std::uint64_t requirements_revision = 0;
    GlobalIndex sample_index{};
    std::size_t block_size = 0;
    std::size_t count = 0;
    bool measure_capture = false;
    std::uint64_t capture_ns = 0;
    std::array<RawObservationValue, maximum_live_observation_probes> values{};
};

struct ObservationCollectRequest {
    std::optional<LiveObservationFrame> frame{};
    bool handled = false;
};

// One realtime writer and one control-path collector. Each owns a private
// buffer; an atomic exchange transfers the third buffer. Unread frames may be
// replaced. A reader never examines storage that the writer can modify.
class LiveObservationHandoff {
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static constexpr unsigned dirty = 4;
    std::array<LiveObservationFrame, 3> frames_{};
    std::atomic<unsigned> middle_{1};
    unsigned writer_ = 0;
    unsigned reader_ = 2;
public:
    LiveObservationFrame& begin(CompiledObservationPlan const& plan,
        std::uint64_t activation, std::uint64_t generation,
        GlobalIndex index, std::size_t block_size) noexcept
    {
        auto& frame = frames_[writer_];
        frame.activation_id = activation;
        frame.project_generation = generation;
        frame.configured_generation = generation;
        frame.requirements_revision = plan.revision;
        frame.sample_index = index;
        frame.block_size = block_size;
        frame.count = plan.probes.size();
        frame.capture_ns = 0;
        frame.measure_capture = false;
        for (std::size_t slot = 0; slot < frame.count; ++slot) {
            auto const& probe = plan.probes[slot];
            frame.values[slot] = {
                .request_id = probe.id,
                .operation = probe.operation,
                .status = probe.available ? ObservationValueStatus::available
                    : ObservationValueStatus::unavailable,
            };
        }
        return frame;
    }
    void publish() noexcept
    {
        writer_ = middle_.exchange(writer_ | dirty, std::memory_order_acq_rel) & ~dirty;
    }
    // The borrowed frame remains valid until this collector's next successful
    // collect(). No audio-thread pointer or mutable NodeStorage escapes here.
    LiveObservationFrame const* collect() noexcept
    {
        if (!(middle_.load(std::memory_order_acquire) & dirty)) return nullptr;
        reader_ = middle_.exchange(reader_, std::memory_order_acq_rel) & ~dirty;
        return &frames_[reader_];
    }
};

} // namespace iv
