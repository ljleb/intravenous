#pragma once

#include <intravenous/observation.h>
#include <intravenous/graph/port_ids.h>
#include <intravenous/runtime/project_connection_types.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace iv {

inline constexpr std::size_t maximum_live_observation_probes = 256;
inline constexpr std::size_t maximum_observation_windows = 256;
inline constexpr std::size_t maximum_observation_interests =
    maximum_live_observation_probes + maximum_observation_windows;

enum class ObservationSourceKind : std::uint8_t { output, input, state_field };
struct ObservationSource {
    NodeAddress node{};
    ObservationSourceKind kind = ObservationSourceKind::output;
    std::string member{};
    std::optional<std::size_t> channel{};
    PortKind port_kind = PortKind::sample;
    auto operator<=>(ObservationSource const&) const = default;
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
    std::vector<SampleOutputChannelId> output_sources{};
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

inline constexpr std::size_t maximum_observation_window_points = 1024;
inline constexpr std::size_t maximum_observation_window_values = 2048;
struct ObservationWindow {
    GlobalIndex begin{};
    std::uint64_t length = 0;
    std::size_t points = 0;
    std::optional<std::uint8_t> max_depth{};
    bool operator==(ObservationWindow const&) const = default;
};
struct ObservationWindowRegion {
    GlobalIndexRange range{};
    std::vector<std::pair<GlobalIndex, ObservationCaptureState>> values{};
    // Projection-local identity within the result's semantic and data version.
    std::vector<std::size_t> region_key{};
};
struct ObservationWindowResult {
    enum class Status { ready, pending_execution, unavailable, failed } status = Status::pending_execution;
    std::uint64_t configured_generation = 0;
    std::uint64_t execution_generation = 0;
    std::uint64_t semantic_version = 0;
    std::uint64_t data_version = 0;
    double sample_rate = 0;
    std::int32_t pace_exponent = 0;
    bool truncated = false;
    std::vector<ObservationWindowRegion> regions{};
    std::vector<TimedEvent> events{};
    std::vector<GlobalIndexRange> unavailable_regions{};
    std::string error{};
};
// One background writer and one control reader. Priority and cancellation
// can change while queued. Cancellation leaves a running transaction intact.
struct ObservationWindowQuery {
    std::uint64_t configured_generation = 0;
    NodeBundlePortId port{};
    ObservationSourceKind kind = ObservationSourceKind::output;
    std::size_t channel = 0;
    std::vector<SampleOutputChannelId> output_sources{};
    ObservationOperation const* operation = nullptr;
    ObservationWindow window{};
    std::atomic<bool> prefetch{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> complete{false};
    ObservationWindowResult result{};
};

struct ObservationCollectRequest {
    std::optional<LiveObservationFrame> frame{};
    std::vector<std::shared_ptr<ObservationWindowQuery>> windows{};
    std::uint64_t execution_data_version = 0;
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
