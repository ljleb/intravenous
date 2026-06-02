#pragma once

#include <intravenous/basic_lane_nodes/type_erased.h>
#include <intravenous/query/lane_query_schema.h>
#include <intravenous/runtime/lane_graph.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <variant>
#include <memory>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// actually this class is INSANELY HUGE
// this needs to be split into smaller parts and then composed together
// again, here we need each class to be self-contained and encapsulated. think hard, don't half-ass the separation because it will come back in your face (yuck)
namespace iv {
    struct LanePortId {
        LanePortDomain domain = LanePortDomain::compiled;
        PortKind kind = PortKind::sample;
        size_t ordinal = 0;

        bool operator==(LanePortId const&) const = default;
    };

    inline LanePortId realtime_sample_input(size_t ordinal = 0)
    {
        return LanePortId {
            .domain = LanePortDomain::realtime,
            .kind = PortKind::sample,
            .ordinal = ordinal,
        };
    }

    inline LanePortId realtime_event_input(size_t ordinal = 0)
    {
        return LanePortId {
            .domain = LanePortDomain::realtime,
            .kind = PortKind::event,
            .ordinal = ordinal,
        };
    }

    struct LaneInputConnection {
        LaneId source {};
        LanePortId input {};

        bool operator==(LaneInputConnection const&) const = default;
    };

    struct LaneOutputConnection {
        LaneId target {};
        LanePortId input {};

        bool operator==(LaneOutputConnection const&) const = default;
    };

    struct LaneGraphConnection {
        LaneId source {};
        LaneId target {};
        LanePortId input {};

        bool operator==(LaneGraphConnection const&) const = default;
    };

    struct LaneRegenerationRequest {
        LaneId lane {};
        TimelineOutputRequest span {};

        bool operator==(LaneRegenerationRequest const&) const = default;
    };

    struct LaneMetadata {
        std::unordered_set<std::string> unit_values {};
        std::unordered_map<std::string, int> int_values {};
        std::unordered_map<std::string, float> float_values {};

        void set_unit(std::string key)
        {
            int_values.erase(key);
            float_values.erase(key);
            unit_values.insert(std::move(key));
        }

        void set_int(std::string key, int value)
        {
            unit_values.erase(key);
            float_values.erase(key);
            int_values[std::move(key)] = value;
        }

        void set_float(std::string key, float value)
        {
            unit_values.erase(key);
            int_values.erase(key);
            float_values[std::move(key)] = value;
        }

        [[nodiscard]] bool has_unit(std::string_view key) const
        {
            return unit_values.contains(std::string(key));
        }

        [[nodiscard]] std::optional<int> int_value(std::string_view key) const
        {
            auto const it = int_values.find(std::string(key));
            if (it == int_values.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        [[nodiscard]] std::optional<float> float_value(std::string_view key) const
        {
            auto const it = float_values.find(std::string(key));
            if (it == float_values.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        [[nodiscard]] std::vector<std::pair<std::string, query::LaneQueryValueType>> schema_entries() const
        {
            std::vector<std::pair<std::string, query::LaneQueryValueType>> entries;
            entries.reserve(unit_values.size() + int_values.size() + float_values.size());
            for (auto const &key : unit_values) {
                entries.emplace_back(key, query::LaneQueryValueType::unit);
            }
            for (auto const &[key, _] : int_values) {
                entries.emplace_back(key, query::LaneQueryValueType::int_);
            }
            for (auto const &[key, _] : float_values) {
                entries.emplace_back(key, query::LaneQueryValueType::float_);
            }
            return entries;
        }
    };

    inline LanePortDomain lane_output_domain(LaneOutputConfig const& output)
    {
        return std::visit([](auto const& config) {
            using Config = std::remove_cvref_t<decltype(config)>;
            if constexpr (
                std::same_as<Config, CompiledSampleLaneOutputConfig>
                || std::same_as<Config, CompiledEventLaneOutputConfig>
            ) {
                return LanePortDomain::compiled;
            } else {
                return LanePortDomain::realtime;
            }
        }, output);
    }

    inline PortKind lane_output_kind(LaneOutputConfig const& output)
    {
        return std::visit([](auto const& config) {
            using Config = std::remove_cvref_t<decltype(config)>;
            if constexpr (
                std::same_as<Config, CompiledEventLaneOutputConfig>
                || std::same_as<Config, RealtimeEventLaneOutputConfig>
            ) {
                return PortKind::event;
            } else {
                return PortKind::sample;
            }
        }, output);
    }

    class LaneGraph;

    struct CompiledLaneOutputStorage {
        std::vector<Sample> samples {};
        std::vector<TimedEvent> events {};
        std::vector<TimelineOutputRequest> invalid_spans {};
    };

    struct RealtimeLaneOutputScratch {
        std::span<Sample> samples {};
        std::vector<TimedEvent> events {};
        size_t sample_start_index = 0;
        size_t sample_count = 0;
        bool sample_valid = false;
        size_t event_start_index = 0;
        size_t event_count = 0;
        bool event_valid = false;
    };

    struct LaneRecord {
        LaneId id {};
        TypeErasedLaneNode node {};
        LaneOutputConfig output {};
        LaneMetadata metadata {};
        bool dirty = false;
        uint64_t invalidation_generation = 0;
    };

    struct CompiledLaneRecord {
        LaneRecord lane {};
        CompiledLaneOutputStorage storage {};
    };

    struct RealtimeLaneRecord {
        LaneRecord lane {};
        RealtimeLaneOutputScratch scratch {};
    };

    struct RemovedLaneConnection {
        LaneGraphConnection connection {};
        bool source_removed = false;
        bool target_removed = false;
    };

    struct LaneLocation {
        LanePortDomain domain = LanePortDomain::compiled;
        size_t index = 0;
    };

    struct LaneGraphDomainConnections {
        std::unordered_map<LaneId, std::vector<LaneInputConnection>, LaneIdHash> inputs_by_target;
        std::unordered_map<LaneId, std::vector<LaneOutputConnection>, LaneIdHash> outputs_by_source;
    };

    struct RealtimeSampleInputBinding {
        LaneGraph* graph = nullptr;
        LaneId target {};
        LanePortId input {};
        std::unordered_set<LaneId, LaneIdHash>* active = nullptr;
    };

    struct RealtimeEventInputBinding {
        LaneGraph* graph = nullptr;
        LaneId target {};
        LanePortId input {};
        std::unordered_set<LaneId, LaneIdHash>* active = nullptr;
        std::vector<TimedEvent> events {};
    };

    struct RealtimeLaneScratchPage {
        std::unique_ptr<Sample[]> samples {};
        size_t capacity = 0;
    };

    class RealtimeLaneScratchRegion {
        static constexpr size_t page_size_bytes = 4096;
        static constexpr size_t page_size_samples = std::max<size_t>(1, page_size_bytes / sizeof(Sample));

        std::vector<RealtimeLaneScratchPage> _pages;
        size_t _page_index = 0;
        size_t _sample_offset = 0;

    public:
        void reset()
        {
            _page_index = 0;
            _sample_offset = 0;
        }

        std::span<Sample> allocate(size_t count)
        {
            if (count == 0) {
                return {};
            }

            if (_pages.empty()) {
                size_t const capacity = std::max(page_size_samples, count);
                _pages.push_back(RealtimeLaneScratchPage {
                    .samples = std::make_unique<Sample[]>(capacity),
                    .capacity = capacity,
                });
            } else if (_page_index >= _pages.size() || _pages[_page_index].capacity - _sample_offset < count) {
                if (_page_index < _pages.size()) {
                    ++_page_index;
                }
                _sample_offset = 0;
                if (_page_index >= _pages.size()) {
                    size_t const capacity = std::max(page_size_samples, count);
                    _pages.push_back(RealtimeLaneScratchPage {
                        .samples = std::make_unique<Sample[]>(capacity),
                        .capacity = capacity,
                    });
                } else if (_pages[_page_index].capacity < count) {
                    size_t const capacity = std::max(page_size_samples, count);
                    _pages[_page_index] = RealtimeLaneScratchPage {
                        .samples = std::make_unique<Sample[]>(capacity),
                        .capacity = capacity,
                    };
                }
            }

            auto& page = _pages[_page_index];
            std::span<Sample> allocation(page.samples.get() + _sample_offset, count);
            _sample_offset += count;
            return allocation;
        }
    };

    struct LaneGraphLaneStore {
        LaneIdAllocator ids;
        std::vector<CompiledLaneRecord> compiled;
        std::vector<RealtimeLaneRecord> realtime;
        std::unordered_map<LaneId, LaneLocation, LaneIdHash> indices;
    };

    struct LaneGraphDomainConnectionStore {
        LaneGraphDomainConnections compiled;
        LaneGraphDomainConnections realtime;
    };

    struct LaneGraphRealtimeCache {
        RealtimeLaneScratchRegion scratch_region;
        size_t sample_start_index = 0;
        size_t sample_count = 0;
        bool sample_window_valid = false;
        size_t sample_block_leases = 0;
        bool sample_cache_reset_pending = false;
    };

    struct LaneGraphHierarchy {
        std::unordered_map<LaneId, std::vector<LaneId>, LaneIdHash> children_by_parent;
        std::unordered_map<LaneId, std::vector<LaneId>, LaneIdHash> parents_by_child;
    };

    struct LaneGraphInvalidationState {
        std::vector<LaneRegenerationRequest> compiled_regeneration_queue;
        std::vector<RemovedLaneConnection> removed_connections;
        uint64_t generation = 0;
    };

    class RealtimeSampleBlockLease {
        LaneGraph* _owner = nullptr;
        std::span<Sample const> _samples {};

        friend class LaneGraph;

        RealtimeSampleBlockLease(LaneGraph& owner, std::span<Sample const> samples) :
            _owner(&owner),
            _samples(samples)
        {}

    public:
        RealtimeSampleBlockLease() = default;
        RealtimeSampleBlockLease(RealtimeSampleBlockLease const&) = delete;
        RealtimeSampleBlockLease& operator=(RealtimeSampleBlockLease const&) = delete;

        RealtimeSampleBlockLease(RealtimeSampleBlockLease&& other) noexcept :
            _owner(std::exchange(other._owner, nullptr)),
            _samples(std::exchange(other._samples, {}))
        {}

        RealtimeSampleBlockLease& operator=(RealtimeSampleBlockLease&& other) noexcept
        {
            if (this == &other) {
                return *this;
            }
            release();
            _owner = std::exchange(other._owner, nullptr);
            _samples = std::exchange(other._samples, {});
            return *this;
        }

        ~RealtimeSampleBlockLease()
        {
            release();
        }

        std::span<Sample const> samples() const
        {
            return _samples;
        }

        void release();
    };

    class LaneGraph {
        friend class RealtimeSampleBlockLease;

    public:

    private:
        LaneGraphLaneStore _lanes;
        LaneGraphDomainConnectionStore _connections;
        LaneGraphRealtimeCache _realtime;
        LaneGraphHierarchy _hierarchy;
        LaneGraphInvalidationState _invalidation;

        static std::vector<LaneInputConnection> const& empty_inputs()
        {
            static std::vector<LaneInputConnection> const empty;
            return empty;
        }

        static std::vector<LaneOutputConnection> const& empty_outputs()
        {
            static std::vector<LaneOutputConnection> const empty;
            return empty;
        }

        static std::vector<LaneId> const& empty_children()
        {
            static std::vector<LaneId> const empty;
            return empty;
        }

        LaneLocation location_for(LaneId id) const
        {
            auto const it = _lanes.indices.find(id);
            if (it == _lanes.indices.end()) {
                throw std::runtime_error("unknown lane id");
            }
            return it->second;
        }

        static LaneGraphDomainConnections& connections_for_domain(
            LanePortDomain domain,
            LaneGraphDomainConnections& compiled,
            LaneGraphDomainConnections& realtime
        )
        {
            return domain == LanePortDomain::compiled ? compiled : realtime;
        }

        static LaneGraphDomainConnections const& connections_for_domain(
            LanePortDomain domain,
            LaneGraphDomainConnections const& compiled,
            LaneGraphDomainConnections const& realtime
        )
        {
            return domain == LanePortDomain::compiled ? compiled : realtime;
        }

        static std::span<Sample const> get_realtime_sample_input_block(
            void* context,
            size_t start_index,
            size_t count
        )
        {
            auto& binding = *static_cast<RealtimeSampleInputBinding*>(context);
            return binding.graph->pull_realtime_sample_input_block(
                binding.target,
                binding.input,
                start_index,
                count,
                *binding.active
            );
        }

        static std::span<TimedEvent const> get_realtime_event_input_block(
            void* context,
            size_t start_index,
            size_t count
        )
        {
            auto& binding = *static_cast<RealtimeEventInputBinding*>(context);
            return binding.graph->pull_realtime_event_input_block(
                binding.target,
                binding.input,
                start_index,
                count,
                *binding.active,
                binding.events
            );
        }

        static size_t input_count(TypeErasedLaneNode const& node, LanePortId input)
        {
            if (input.domain == LanePortDomain::compiled && input.kind == PortKind::sample) {
                return node.compiled_sample_inputs().size();
            }
            if (input.domain == LanePortDomain::compiled && input.kind == PortKind::event) {
                return node.compiled_event_inputs().size();
            }
            if (input.domain == LanePortDomain::realtime && input.kind == PortKind::sample) {
                return node.realtime_sample_inputs().size();
            }
            return node.realtime_event_inputs().size();
        }

        void validate_connection(LaneId source, LaneId target, LanePortId input) const
        {
            auto const& source_record = lane(source);
            auto const& target_record = lane(target);
            if (lane_output_kind(source_record.output) != input.kind) {
                throw std::runtime_error("lane connection kind mismatch");
            }
            if (lane_output_domain(source_record.output) != input.domain) {
                throw std::runtime_error("lane connection domain mismatch");
            }
            if (input.ordinal >= input_count(target_record.node, input)) {
                throw std::runtime_error("lane connection target input ordinal out of range");
            }
        }

        // I don't know what is going on here but it's just insane how complex this class is
        // and how many responsibilities it was forced to do
        // it is in pain, crying, it wants to stop living
        // give it a SINGLE JOB not 40
    public:
        LaneGraph() = default;
        LaneGraph(LaneGraph const&) = delete;
        LaneGraph& operator=(LaneGraph const&) = delete;
        LaneGraph(LaneGraph&&) = delete;
        LaneGraph& operator=(LaneGraph&&) = delete;

        LaneId add_lane(TypeErasedLaneNode node, LaneMetadata metadata = {})
        {
            LaneId const id = _lanes.ids.next();
            LaneOutputConfig output = node.output();
            auto record = LaneRecord {
                .id = id,
                .node = std::move(node),
                .output = std::move(output),
                .metadata = std::move(metadata),
            };
            LanePortDomain const domain = lane_output_domain(record.output);
            if (domain == LanePortDomain::compiled) {
                _lanes.indices.emplace(id, LaneLocation {
                    .domain = domain,
                    .index = _lanes.compiled.size(),
                });
                _lanes.compiled.push_back(CompiledLaneRecord {
                    .lane = std::move(record),
                });
            } else {
                _lanes.indices.emplace(id, LaneLocation {
                    .domain = domain,
                    .index = _lanes.realtime.size(),
                });
                _lanes.realtime.push_back(RealtimeLaneRecord {
                    .lane = std::move(record),
                });
            }
            return id;
        }

        void upsert_lane(LaneId id, TypeErasedLaneNode node, LaneMetadata metadata = {})
        {
            LaneOutputConfig output = node.output();
            LanePortDomain const domain = lane_output_domain(output);
            if (contains(id)) {
                auto const location = location_for(id);
                if (location.domain != domain) {
                    throw std::runtime_error("lane upsert cannot change output domain");
                }
                auto& record = lane(id);
                record.node = std::move(node);
                record.output = std::move(output);
                record.metadata = std::move(metadata);
                clear_realtime_sample_caches();
                return;
            }

            _lanes.ids.observe(id);
            auto record = LaneRecord {
                .id = id,
                .node = std::move(node),
                .output = std::move(output),
                .metadata = std::move(metadata),
            };
            if (domain == LanePortDomain::compiled) {
                _lanes.indices.emplace(id, LaneLocation {
                    .domain = domain,
                    .index = _lanes.compiled.size(),
                });
                _lanes.compiled.push_back(CompiledLaneRecord {
                    .lane = std::move(record),
                });
            } else {
                _lanes.indices.emplace(id, LaneLocation {
                    .domain = domain,
                    .index = _lanes.realtime.size(),
                });
                _lanes.realtime.push_back(RealtimeLaneRecord {
                    .lane = std::move(record),
                });
            }
        }

        bool contains(LaneId id) const
        {
            return _lanes.indices.contains(id);
        }

        LaneRecord const& lane(LaneId id) const
        {
            auto const location = location_for(id);
            return location.domain == LanePortDomain::compiled
                ? _lanes.compiled[location.index].lane
                : _lanes.realtime[location.index].lane;
        }

        LaneRecord& lane(LaneId id)
        {
            auto const location = location_for(id);
            return location.domain == LanePortDomain::compiled
                ? _lanes.compiled[location.index].lane
                : _lanes.realtime[location.index].lane;
        }

        CompiledLaneRecord const& compiled_lane(LaneId id) const
        {
            auto const location = location_for(id);
            if (location.domain != LanePortDomain::compiled) {
                throw std::runtime_error("lane is not a compiled-output lane");
            }
            return _lanes.compiled[location.index];
        }

        CompiledLaneRecord& compiled_lane(LaneId id)
        {
            auto const location = location_for(id);
            if (location.domain != LanePortDomain::compiled) {
                throw std::runtime_error("lane is not a compiled-output lane");
            }
            return _lanes.compiled[location.index];
        }

        RealtimeLaneRecord const& realtime_lane(LaneId id) const
        {
            auto const location = location_for(id);
            if (location.domain != LanePortDomain::realtime) {
                throw std::runtime_error("lane is not a realtime-output lane");
            }
            return _lanes.realtime[location.index];
        }

        RealtimeLaneRecord& realtime_lane(LaneId id)
        {
            auto const location = location_for(id);
            if (location.domain != LanePortDomain::realtime) {
                throw std::runtime_error("lane is not a realtime-output lane");
            }
            return _lanes.realtime[location.index];
        }

        std::vector<CompiledLaneRecord> const& compiled_lanes() const
        {
            return _lanes.compiled;
        }

        std::vector<RealtimeLaneRecord> const& realtime_lanes() const
        {
            return _lanes.realtime;
        }

        size_t lane_count() const
        {
            return _lanes.compiled.size() + _lanes.realtime.size();
        }

        template<typename Fn>
        void for_each_lane(Fn&& fn) const
        {
            for (auto const& record : _lanes.compiled) {
                std::invoke(fn, record.lane);
            }
            for (auto const& record : _lanes.realtime) {
                std::invoke(fn, record.lane);
            }
        }

        template<typename Fn>
        void for_each_lane(Fn&& fn)
        {
            for (auto& record : _lanes.compiled) {
                std::invoke(fn, record.lane);
            }
            for (auto& record : _lanes.realtime) {
                std::invoke(fn, record.lane);
            }
        }

        std::vector<RemovedLaneConnection> const& removed_connections() const
        {
            return _invalidation.removed_connections;
        }

        void clear_removed_connections()
        {
            _invalidation.removed_connections.clear();
        }

        void connect(LaneId source, LaneId target, LanePortId input)
        {
            validate_connection(source, target, input);
            clear_realtime_sample_caches();

            LaneInputConnection input_connection {
                .source = source,
                .input = input,
            };
            auto& connections = connections_for_domain(input.domain, _connections.compiled, _connections.realtime);
            auto& inputs = connections.inputs_by_target[target];
            if (std::ranges::find(inputs, input_connection) == inputs.end()) {
                inputs.push_back(input_connection);
            }

            LaneOutputConnection output_connection {
                .target = target,
                .input = input,
            };
            auto& outputs = connections.outputs_by_source[source];
            if (std::ranges::find(outputs, output_connection) == outputs.end()) {
                outputs.push_back(output_connection);
            }
        }

        void disconnect(LaneId source, LaneId target, LanePortId input)
        {
            if (!contains(source) || !contains(target)) {
                return;
            }
            clear_realtime_sample_caches();

            auto& connections = connections_for_domain(input.domain, _connections.compiled, _connections.realtime);
            if (auto it = connections.inputs_by_target.find(target); it != connections.inputs_by_target.end()) {
                auto& inputs = it->second;
                std::erase(inputs, LaneInputConnection { .source = source, .input = input });
                if (inputs.empty()) {
                    connections.inputs_by_target.erase(it);
                }
            }

            if (auto it = connections.outputs_by_source.find(source); it != connections.outputs_by_source.end()) {
                auto& outputs = it->second;
                std::erase(outputs, LaneOutputConnection { .target = target, .input = input });
                if (outputs.empty()) {
                    connections.outputs_by_source.erase(it);
                }
            }
        }

        void disconnect_input(LaneId target, LanePortId input)
        {
            if (!contains(target)) {
                return;
            }
            auto const inputs = inputs_for(target);
            for (auto const& connection : inputs) {
                if (connection.input == input) {
                    disconnect(connection.source, target, input);
                }
            }
        }

        void connect_exclusive(LaneId source, LaneId target, LanePortId input)
        {
            disconnect_input(target, input);
            connect(source, target, input);
        }

        void add_child(LaneId parent, LaneId child)
        {
            (void)location_for(parent);
            (void)location_for(child);

            auto& children = _hierarchy.children_by_parent[parent];
            if (std::ranges::find(children, child) == children.end()) {
                children.push_back(child);
            }

            auto& parents = _hierarchy.parents_by_child[child];
            if (std::ranges::find(parents, parent) == parents.end()) {
                parents.push_back(parent);
            }
        }

        void remove_child(LaneId parent, LaneId child)
        {
            if (auto it = _hierarchy.children_by_parent.find(parent); it != _hierarchy.children_by_parent.end()) {
                std::erase(it->second, child);
                if (it->second.empty()) {
                    _hierarchy.children_by_parent.erase(it);
                }
            }

            if (auto it = _hierarchy.parents_by_child.find(child); it != _hierarchy.parents_by_child.end()) {
                std::erase(it->second, parent);
                if (it->second.empty()) {
                    _hierarchy.parents_by_child.erase(it);
                }
            }
        }

        void remove_lane(LaneId removed)
        {
            if (!contains(removed)) {
                return;
            }

            auto remove_outputs = [&](LaneGraphDomainConnections& connections) {
                auto it = connections.outputs_by_source.find(removed);
                if (it == connections.outputs_by_source.end()) {
                    return;
                }
                auto outputs = std::move(it->second);
                connections.outputs_by_source.erase(it);
                for (auto const& output : outputs) {
                    _invalidation.removed_connections.push_back(RemovedLaneConnection {
                        .connection = LaneGraphConnection {
                            .source = removed,
                            .target = output.target,
                            .input = output.input,
                        },
                        .source_removed = true,
                    });
                    if (auto target_it = connections.inputs_by_target.find(output.target);
                        target_it != connections.inputs_by_target.end()
                    ) {
                        auto& target_inputs = target_it->second;
                        std::erase(target_inputs, LaneInputConnection {
                            .source = removed,
                            .input = output.input,
                        });
                        if (target_inputs.empty()) {
                            connections.inputs_by_target.erase(target_it);
                        }
                    }
                    if (
                        contains(output.target)
                        && lane_output_domain(lane(output.target).output) == LanePortDomain::compiled
                    ) {
                        auto& target = lane(output.target);
                        target.dirty = true;
                        target.invalidation_generation = ++_invalidation.generation;
                        compiled_lane(output.target).storage.invalid_spans.push_back(TimelineOutputRequest {});
                        LaneRegenerationRequest const request {
                            .lane = output.target,
                            .span = TimelineOutputRequest {},
                        };
                        if (std::ranges::find(_invalidation.compiled_regeneration_queue, request) == _invalidation.compiled_regeneration_queue.end()) {
                            _invalidation.compiled_regeneration_queue.push_back(request);
                        }
                    }
                }
            };
            remove_outputs(_connections.compiled);
            remove_outputs(_connections.realtime);

            auto remove_inputs = [&](LaneGraphDomainConnections& connections) {
                auto it = connections.inputs_by_target.find(removed);
                if (it == connections.inputs_by_target.end()) {
                    return;
                }
                auto inputs = std::move(it->second);
                connections.inputs_by_target.erase(it);
                for (auto const& input : inputs) {
                    _invalidation.removed_connections.push_back(RemovedLaneConnection {
                        .connection = LaneGraphConnection {
                            .source = input.source,
                            .target = removed,
                            .input = input.input,
                        },
                        .target_removed = true,
                    });
                    if (auto source_it = connections.outputs_by_source.find(input.source);
                        source_it != connections.outputs_by_source.end()
                    ) {
                        auto& source_outputs = source_it->second;
                        std::erase(source_outputs, LaneOutputConnection {
                            .target = removed,
                            .input = input.input,
                        });
                        if (source_outputs.empty()) {
                            connections.outputs_by_source.erase(source_it);
                        }
                    }
                }
            };
            remove_inputs(_connections.compiled);
            remove_inputs(_connections.realtime);

            if (auto it = _hierarchy.children_by_parent.find(removed); it != _hierarchy.children_by_parent.end()) {
                auto children = std::move(it->second);
                _hierarchy.children_by_parent.erase(it);
                for (auto const& child : children) {
                    remove_child(removed, child);
                }
            }

            if (auto it = _hierarchy.parents_by_child.find(removed); it != _hierarchy.parents_by_child.end()) {
                auto parents = std::move(it->second);
                _hierarchy.parents_by_child.erase(it);
                for (auto const& parent : parents) {
                    remove_child(parent, removed);
                }
            }

            auto const location = location_for(removed);
            if (location.domain == LanePortDomain::compiled) {
                size_t const last_index = _lanes.compiled.size() - 1;
                if (location.index != last_index) {
                    std::swap(_lanes.compiled[location.index], _lanes.compiled[last_index]);
                    _lanes.indices[_lanes.compiled[location.index].lane.id] = LaneLocation {
                        .domain = LanePortDomain::compiled,
                        .index = location.index,
                    };
                }
                _lanes.compiled.pop_back();
            } else {
                size_t const last_index = _lanes.realtime.size() - 1;
                if (location.index != last_index) {
                    std::swap(_lanes.realtime[location.index], _lanes.realtime[last_index]);
                    _lanes.indices[_lanes.realtime[location.index].lane.id] = LaneLocation {
                        .domain = LanePortDomain::realtime,
                        .index = location.index,
                    };
                }
                _lanes.realtime.pop_back();
            }
            _lanes.indices.erase(removed);

            _invalidation.compiled_regeneration_queue.erase(
                std::remove_if(
                    _invalidation.compiled_regeneration_queue.begin(),
                    _invalidation.compiled_regeneration_queue.end(),
                    [&](LaneRegenerationRequest const& request) {
                        return request.lane == removed;
                    }
                ),
                _invalidation.compiled_regeneration_queue.end()
            );
        }

        std::vector<LaneInputConnection> const& inputs_for(LaneId target) const
        {
            if (auto const it = _connections.realtime.inputs_by_target.find(target);
                it != _connections.realtime.inputs_by_target.end()
            ) {
                return it->second;
            }
            auto const it = _connections.compiled.inputs_by_target.find(target);
            return it == _connections.compiled.inputs_by_target.end() ? empty_inputs() : it->second;
        }

        std::vector<LaneOutputConnection> const& outputs_for(LaneId source) const
        {
            if (!contains(source)) {
                return empty_outputs();
            }
            LanePortDomain const domain = lane_output_domain(lane(source).output);
            auto const& connections = connections_for_domain(
                domain,
                _connections.compiled,
                _connections.realtime
            );
            auto const it = connections.outputs_by_source.find(source);
            return it == connections.outputs_by_source.end() ? empty_outputs() : it->second;
        }

        std::vector<LaneInputConnection> const& compiled_inputs_for(LaneId target) const
        {
            auto const it = _connections.compiled.inputs_by_target.find(target);
            return it == _connections.compiled.inputs_by_target.end() ? empty_inputs() : it->second;
        }

        std::vector<LaneOutputConnection> const& compiled_outputs_for(LaneId source) const
        {
            auto const it = _connections.compiled.outputs_by_source.find(source);
            return it == _connections.compiled.outputs_by_source.end() ? empty_outputs() : it->second;
        }

        std::vector<LaneInputConnection> const& realtime_inputs_for(LaneId target) const
        {
            auto const it = _connections.realtime.inputs_by_target.find(target);
            return it == _connections.realtime.inputs_by_target.end() ? empty_inputs() : it->second;
        }

        std::vector<LaneOutputConnection> const& realtime_outputs_for(LaneId source) const
        {
            auto const it = _connections.realtime.outputs_by_source.find(source);
            return it == _connections.realtime.outputs_by_source.end() ? empty_outputs() : it->second;
        }

        std::vector<LaneId> const& children_for(LaneId parent) const
        {
            auto const it = _hierarchy.children_by_parent.find(parent);
            return it == _hierarchy.children_by_parent.end() ? empty_children() : it->second;
        }

        std::vector<LaneId> const& parents_for(LaneId child) const
        {
            auto const it = _hierarchy.parents_by_child.find(child);
            return it == _hierarchy.parents_by_child.end() ? empty_children() : it->second;
        }

        std::vector<LaneRegenerationRequest> const& compiled_regeneration_queue() const
        {
            return _invalidation.compiled_regeneration_queue;
        }

        void clear_compiled_regeneration_queue()
        {
            _invalidation.compiled_regeneration_queue.clear();
        }

        void clear_realtime_sample_caches()
        {
            if (_realtime.sample_block_leases != 0) {
                _realtime.sample_cache_reset_pending = true;
                _realtime.sample_window_valid = false;
                for (auto& record : _lanes.realtime) {
                    record.scratch.sample_valid = false;
                    record.scratch.samples = {};
                    record.scratch.event_valid = false;
                }
                return;
            }
            reset_realtime_sample_cache_storage();
        }

    private:
        void reset_realtime_sample_cache_storage()
        {
            _realtime.scratch_region.reset();
            _realtime.sample_window_valid = false;
            _realtime.sample_cache_reset_pending = false;
            for (auto& record : _lanes.realtime) {
                record.scratch.sample_valid = false;
                record.scratch.samples = {};
                record.scratch.event_valid = false;
            }
        }

        void release_realtime_sample_block()
        {
            if (_realtime.sample_block_leases == 0) {
                return;
            }
            --_realtime.sample_block_leases;
            if (_realtime.sample_block_leases == 0 && _realtime.sample_cache_reset_pending) {
                reset_realtime_sample_cache_storage();
            }
        }

    public:
        void mark_dirty_cascade(LaneId changed)
        {
            mark_dirty_cascade(changed, TimelineOutputRequest {});
        }

        void mark_dirty_cascade(LaneId changed, TimelineOutputRequest span)
        {
            (void)location_for(changed);

            ++_invalidation.generation;
            std::unordered_set<LaneId, LaneIdHash> visited;
            std::unordered_set<LaneId, LaneIdHash> queued_compiled;
            std::queue<LaneId> pending;
            pending.push(changed);

            while (!pending.empty()) {
                LaneId const current = pending.front();
                pending.pop();
                if (!visited.insert(current).second) {
                    continue;
                }

                auto& record = lane(current);
                record.dirty = true;
                record.invalidation_generation = _invalidation.generation;
                if (
                    lane_output_domain(record.output) == LanePortDomain::compiled
                    && queued_compiled.insert(current).second
                ) {
                    compiled_lane(current).storage.invalid_spans.push_back(span);
                    _invalidation.compiled_regeneration_queue.push_back(LaneRegenerationRequest {
                        .lane = current,
                        .span = span,
                    });
                }

                for (auto const& output : outputs_for(current)) {
                    pending.push(output.target);
                }
            }
        }

        void pull_realtime_samples(LaneId source, size_t start_index, std::span<Sample> out)
        {
            std::unordered_set<LaneId, LaneIdHash> active;
            pull_realtime_samples(source, start_index, out, active);
        }

        RealtimeSampleBlockLease pull_realtime_sample_block(
            LaneId source,
            size_t start_index,
            size_t count
        )
        {
            if (
                !_realtime.sample_window_valid
                || _realtime.sample_start_index != start_index
                || _realtime.sample_count != count
            ) {
                clear_realtime_sample_caches();
                _realtime.sample_start_index = start_index;
                _realtime.sample_count = count;
                _realtime.sample_window_valid = true;
            }
            std::unordered_set<LaneId, LaneIdHash> active;
            auto block = pull_realtime_sample_block(source, start_index, count, active);
            ++_realtime.sample_block_leases;
            return RealtimeSampleBlockLease(*this, block);
        }

        std::span<TimedEvent const> pull_realtime_events(LaneId source, size_t start_index, size_t count)
        {
            std::unordered_set<LaneId, LaneIdHash> active;
            return pull_realtime_events(source, start_index, count, active);
        }

        bool regenerate_compiled_output(LaneId id, TimelineOutputRequest span)
        {
            auto& record = compiled_lane(id);
            LaneOutputView output;
            if (std::holds_alternative<CompiledSampleLaneOutputConfig>(record.lane.output)) {
                output = CompiledSampleLaneOutput { .samples = &record.storage.samples };
            } else if (std::holds_alternative<CompiledEventLaneOutputConfig>(record.lane.output)) {
                record.storage.events.clear();
                output = CompiledEventLaneOutput { .events = &record.storage.events };
            } else {
                throw std::runtime_error("lane does not have a compiled output");
            }

            std::vector<CompiledSampleLaneInput> compiled_sample_inputs(
                record.lane.node.compiled_sample_inputs().size()
            );
            for (size_t input_i = 0; input_i < compiled_sample_inputs.size(); ++input_i) {
                compiled_sample_inputs[input_i].default_value =
                    record.lane.node.compiled_sample_inputs()[input_i].default_value;
            }
            std::vector<CompiledEventLaneInput> compiled_event_inputs(
                record.lane.node.compiled_event_inputs().size()
            );
            for (auto const& connection : compiled_inputs_for(id)) {
                if (connection.input.domain != LanePortDomain::compiled || !contains(connection.source)) {
                    continue;
                }
                if (connection.input.kind == PortKind::sample) {
                    if (connection.input.ordinal >= compiled_sample_inputs.size()) {
                        continue;
                    }
                    auto const& source = compiled_lane(connection.source).storage.samples;
                    compiled_sample_inputs[connection.input.ordinal].sources.push_back(source);
                } else {
                    if (connection.input.ordinal >= compiled_event_inputs.size()) {
                        continue;
                    }
                    auto const& source = compiled_lane(connection.source).storage.events;
                    compiled_event_inputs[connection.input.ordinal].sources.push_back(source);
                }
            }
            UntypedTimelineGenerateContext untyped {
                .output_request = TimelineOutputRequest {
                    .start_index = span.start_index,
                    .count = span.count,
                },
                .compiled_sample_inputs = compiled_sample_inputs,
                .compiled_event_inputs = compiled_event_inputs,
                .output = std::move(output),
            };
            TimelineGenerateContext<TypeErasedLaneNode> ctx(untyped);
            record.lane.node.generate(ctx);
            record.lane.dirty = false;
            record.storage.invalid_spans.clear();
            return true;
        }

        size_t regenerate_pending_compiled_outputs(size_t max_count = static_cast<size_t>(-1))
        {
            size_t processed = 0;
            while (!_invalidation.compiled_regeneration_queue.empty() && processed < max_count) {
                auto const request = _invalidation.compiled_regeneration_queue.front();
                _invalidation.compiled_regeneration_queue.erase(_invalidation.compiled_regeneration_queue.begin());
                if (contains(request.lane)) {
                    regenerate_compiled_output(request.lane, request.span);
                    ++processed;
                }
            }
            return processed;
        }

    private:
        std::span<Sample const> pull_realtime_sample_block(
            LaneId source,
            size_t start_index,
            size_t count,
            std::unordered_set<LaneId, LaneIdHash>& active
        )
        {
            auto& record = realtime_lane(source);
            if (
                record.scratch.sample_valid
                && record.scratch.sample_start_index == start_index
                && record.scratch.sample_count == count
            ) {
                return std::span<Sample const>(record.scratch.samples.data(), record.scratch.samples.size());
            }

            record.scratch.samples = _realtime.scratch_region.allocate(count);
            std::span<Sample> out(record.scratch.samples.data(), record.scratch.samples.size());
            pull_realtime_samples(source, start_index, out, active);
            record.scratch.sample_start_index = start_index;
            record.scratch.sample_count = count;
            record.scratch.sample_valid = true;
            return std::span<Sample const>(record.scratch.samples.data(), record.scratch.samples.size());
        }

        std::span<Sample const> pull_realtime_sample_input_block(
            LaneId target,
            LanePortId input,
            size_t start_index,
            size_t count,
            std::unordered_set<LaneId, LaneIdHash>& active
        )
        {
            auto out = _realtime.scratch_region.allocate(count);
            std::ranges::fill(out, 0.0f);
            bool found_source = false;
            for (auto const& connection : realtime_inputs_for(target)) {
                if (connection.input != input) {
                    continue;
                }
                auto const source_block = pull_realtime_sample_block(
                    connection.source,
                    start_index,
                    count,
                    active
                );
                for (size_t i = 0; i < std::min(out.size(), source_block.size()); ++i) {
                    out[i] += source_block[i];
                }
                found_source = true;
            }
            if (!found_source) {
                auto const& config = lane(target).node.realtime_sample_inputs()[input.ordinal];
                std::ranges::fill(out, config.default_value);
            }
            return out;
        }

        std::span<TimedEvent const> pull_realtime_event_input_block(
            LaneId target,
            LanePortId input,
            size_t start_index,
            size_t count,
            std::unordered_set<LaneId, LaneIdHash>& active,
            std::vector<TimedEvent>& out
        )
        {
            out.clear();
            for (auto const& connection : realtime_inputs_for(target)) {
                if (connection.input != input) {
                    continue;
                }
                auto const source_events = pull_realtime_events(
                    connection.source,
                    start_index,
                    count,
                    active
                );
                out.insert(out.end(), source_events.begin(), source_events.end());
            }
            std::ranges::sort(out, {}, &TimedEvent::time);
            return out;
        }

        std::vector<RealtimeEventLaneInput> make_realtime_event_inputs(
            LaneRecord const& lane,
            LaneId source,
            size_t start_index,
            size_t count,
            std::unordered_set<LaneId, LaneIdHash>& active,
            std::vector<RealtimeEventInputBinding>& bindings
        )
        {
            bindings.reserve(lane.node.realtime_event_inputs().size());
            std::vector<RealtimeEventLaneInput> inputs;
            inputs.reserve(lane.node.realtime_event_inputs().size());
            for (size_t input_i = 0; input_i < lane.node.realtime_event_inputs().size(); ++input_i) {
                bindings.push_back(RealtimeEventInputBinding {
                    .graph = this,
                    .target = source,
                    .input = LanePortId {
                        .domain = LanePortDomain::realtime,
                        .kind = PortKind::event,
                        .ordinal = input_i,
                    },
                    .active = &active,
                });
                inputs.push_back(RealtimeEventLaneInput {
                    .context = &bindings.back(),
                    .get_events_fn = &LaneGraph::get_realtime_event_input_block,
                    .active_start_index = start_index,
                    .active_count = count,
                });
            }
            return inputs;
        }

        std::vector<RealtimeSampleLaneInput> make_realtime_sample_inputs(
            LaneRecord const& lane,
            LaneId source,
            size_t start_index,
            size_t count,
            std::unordered_set<LaneId, LaneIdHash>& active,
            std::vector<RealtimeSampleInputBinding>& bindings
        )
        {
            bindings.reserve(lane.node.realtime_sample_inputs().size());
            std::vector<RealtimeSampleLaneInput> inputs;
            inputs.reserve(lane.node.realtime_sample_inputs().size());
            for (size_t input_i = 0; input_i < lane.node.realtime_sample_inputs().size(); ++input_i) {
                bindings.push_back(RealtimeSampleInputBinding {
                    .graph = this,
                    .target = source,
                    .input = LanePortId {
                        .domain = LanePortDomain::realtime,
                        .kind = PortKind::sample,
                        .ordinal = input_i,
                    },
                    .active = &active,
                });
                inputs.push_back(RealtimeSampleLaneInput {
                    .context = &bindings.back(),
                    .get_block_fn = &LaneGraph::get_realtime_sample_input_block,
                    .default_value = lane.node.realtime_sample_inputs()[input_i].default_value,
                    .active_start_index = start_index,
                    .active_count = count,
                    .has_source = std::ranges::any_of(
                        realtime_inputs_for(source),
                        [input_i](LaneInputConnection const& connection) {
                            return connection.input.domain == LanePortDomain::realtime
                                && connection.input.kind == PortKind::sample
                                && connection.input.ordinal == input_i;
                        }
                    ),
                });
            }
            return inputs;
        }

        std::span<TimedEvent const> pull_realtime_events(
            LaneId source,
            size_t start_index,
            size_t count,
            std::unordered_set<LaneId, LaneIdHash>& active
        )
        {
            auto& record = realtime_lane(source);
            if (lane_output_kind(record.lane.output) != PortKind::event) {
                throw std::runtime_error("lane does not have a realtime event output");
            }
            if (
                record.scratch.event_valid
                && record.scratch.event_start_index == start_index
                && record.scratch.event_count == count
            ) {
                return record.scratch.events;
            }
            if (!active.insert(source).second) {
                throw std::runtime_error("cycle while pulling realtime lane events");
            }

            std::vector<RealtimeEventInputBinding> realtime_event_bindings;
            auto realtime_event_inputs = make_realtime_event_inputs(
                record.lane,
                source,
                start_index,
                count,
                active,
                realtime_event_bindings
            );

            record.scratch.events.clear();
            LaneOutputView output = RealtimeEventLaneOutput { record.scratch.events };
            UntypedTimelineGenerateContext untyped {
                .output_request = TimelineOutputRequest {
                    .start_index = start_index,
                    .count = count,
                },
                .realtime_event_inputs = realtime_event_inputs,
                .output = std::move(output),
            };
            TimelineGenerateContext<TypeErasedLaneNode> ctx(untyped);
            record.lane.node.generate(ctx);
            std::ranges::sort(record.scratch.events, {}, &TimedEvent::time);
            record.scratch.event_start_index = start_index;
            record.scratch.event_count = count;
            record.scratch.event_valid = true;
            active.erase(source);
            return record.scratch.events;
        }

        void pull_realtime_samples(
            LaneId source,
            size_t start_index,
            std::span<Sample> out,
            std::unordered_set<LaneId, LaneIdHash>& active
        )
        {
            auto& record = realtime_lane(source);
            if (lane_output_kind(record.lane.output) != PortKind::sample) {
                throw std::runtime_error("lane does not have a realtime sample output");
            }
            if (!active.insert(source).second) {
                throw std::runtime_error("cycle while pulling realtime lane samples");
            }

            std::vector<RealtimeSampleInputBinding> realtime_sample_bindings;
            auto realtime_sample_inputs = make_realtime_sample_inputs(
                record.lane,
                source,
                start_index,
                out.size(),
                active,
                realtime_sample_bindings
            );

            std::ranges::fill(out, 0.0f);
            LaneOutputView output = RealtimeSampleLaneOutput {
                BlockView<Sample> {
                    .first = out,
                },
            };
            UntypedTimelineGenerateContext untyped {
                .output_request = TimelineOutputRequest {
                    .start_index = start_index,
                    .count = out.size(),
                },
                .realtime_sample_inputs = realtime_sample_inputs,
                .output = std::move(output),
            };
            TimelineGenerateContext<TypeErasedLaneNode> ctx(untyped);
            record.lane.node.generate(ctx);
            active.erase(source);
        }
    };

    inline void RealtimeSampleBlockLease::release()
    {
        if (!_owner) {
            return;
        }
        _owner->release_realtime_sample_block();
        _owner = nullptr;
        _samples = {};
    }
}
