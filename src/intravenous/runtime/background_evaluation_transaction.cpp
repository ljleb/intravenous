#include <intravenous/runtime/background_evaluation_transaction.h>

#include <intravenous/runtime/background_evaluation_call_frame.h>
#include <intravenous/runtime/background_storage_realization.h>

#include <algorithm>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {
namespace {

[[nodiscard]] bool contains_coverage(Coverage const& outer,
                                     Coverage const& inner) noexcept
{
    return std::ranges::all_of(inner.regions(), [&](IndexRegion region) {
        return outer.contains(region);
    });
}

[[nodiscard]] Coverage shifted_back(Coverage const& coverage,
                                    std::size_t latency)
{
    Coverage result;
    for (auto const region : coverage.regions()) {
        if (region.end <= latency) continue;
        result.include({
            region.begin < latency ? 0 : region.begin - latency,
            region.end - latency,
        });
    }
    return result;
}

[[nodiscard]] bool
runtime_produced(graph_jit::BackgroundEvaluationPlan const& plan,
                 graph_jit::PortStoragePlan const& storage) noexcept
{
    if (storage.sample_materialization || storage.event_materialization)
        return true;
    if (!storage.output_port || *storage.output_port >= plan.ports.size())
        return false;
    auto const& output = plan.ports[*storage.output_port];
    return output.authored_tock_output || output.replayed_tick_output;
}

[[nodiscard]] std::vector<IndexRegion> replay_regions(Coverage const& coverage,
                                                      std::size_t maximum)
{
    if (maximum == 0 && !coverage.empty()) {
        throw std::invalid_argument(
            "background replay has no compiled block limit");
    }
    std::vector<IndexRegion> result;
    for (auto const region : coverage.regions()) {
        auto begin = region.begin;
        while (begin < region.end) {
            auto const end = std::min(
                region.end, saturating_sample_index_add(
                                begin, static_cast<SampleIndex>(maximum)));
            if (end <= begin) {
                throw std::length_error(
                    "background replay region cannot be split");
            }
            result.push_back({begin, end});
            begin = end;
        }
    }
    return result;
}

} // namespace

class BackgroundEvaluationTransaction::Impl {
    struct PageMutation {
        PersistedOutputId output{};
        std::uint64_t page = 0;
    };

    CompiledGraph const* graph_ = nullptr;
    std::byte* node_storage_ = nullptr;
    BackgroundCoverageState* coverage_ = nullptr;
    BackgroundPropagationWorkspace* propagation_ = nullptr;
    PersistedPageStore* pages_ = nullptr;
    BackgroundEvaluationRequest request_{};
    PersistedPageStore::ReaderSlot reader_{};
    std::optional<PersistedPageStore::ReaderPin> pin_{};
    std::optional<PreparedCoveragePropagation> prepared_{};
    std::vector<PageMutation> page_mutations_{};
    bool executed_ = false;

    void discard_prepared() noexcept
    {
        if (!prepared_) return;
        propagation_->discard(std::move(*prepared_));
        prepared_.reset();
    }

    void release_reader() noexcept
    {
        pin_.reset();
        reader_ = {};
    }

    [[nodiscard]] bool
    node_evaluates(graph_jit::BackgroundNodeIndex node) const noexcept
    {
        auto const activity = prepared_->node_activity();
        return node < activity.size() &&
               graph_jit::has_activity(
                   activity[node], graph_jit::BackgroundNodeActivity::evaluate);
    }

    void add_page_mutation(PersistedOutputId output, std::uint64_t page)
    {
        if (!std::ranges::any_of(
                page_mutations_, [&](PageMutation const& item) {
                    return item.page == page && item.output == output;
                })) {
            page_mutations_.push_back({std::move(output), page});
        }
    }

    static void
    expand_output_demand(void* opaque, graph_jit::BackgroundPortIndex port,
                         Coverage const& available, Coverage const& changed,
                         Coverage const& required, Coverage& additional)
    {
        auto& self = *static_cast<Impl*>(opaque);
        auto const& plan = self.graph_->background_evaluation_plan;
        auto const& snapshot = self.pin_->snapshot();
        auto const width = self.request_.page_width;
        Coverage affected = required | changed;
        if (affected.empty()) return;

        for (graph_jit::PortStorageIndex index = 0;
             index < plan.storage.ports.size(); ++index) {
            auto const& storage = plan.storage.ports[index];
            if (storage.storage !=
                    graph_jit::PortStorageKind::persisted_pages ||
                storage.output_port != port ||
                !runtime_produced(plan, storage)) {
                continue;
            }
            auto const output = persisted_output_id(
                plan, index, self.graph_->project_generation);
            for (auto const region : affected.regions()) {
                auto page = static_cast<std::uint64_t>(region.begin / width);
                auto const last =
                    static_cast<std::uint64_t>((region.end - 1) / width);
                for (;; ++page) {
                    auto const page_begin =
                        static_cast<SampleIndex>(page * width);
                    auto const page_end = saturating_sample_index_add(
                        page_begin, static_cast<SampleIndex>(width));
                    auto const page_region = IndexRegion{page_begin, page_end};
                    auto const desired = available & Coverage{page_region};
                    auto const is_changed = changed.intersects(page_region);
                    Coverage existing_domain;
                    bool exists = false;
                    if (storage.kind == PortKind::sample) {
                        if (auto const* existing =
                                snapshot.find_sample_page(output, page)) {
                            existing_domain = existing->domain;
                            exists = true;
                        }
                    } else if (auto const* existing =
                                   snapshot.find_event_page(output, page)) {
                        existing_domain = existing->domain;
                        exists = true;
                    }
                    if (is_changed ||
                        (required.intersects(page_region) &&
                         (!exists ||
                          !contains_coverage(existing_domain, desired)))) {
                        additional.include(desired);
                    }
                    if (is_changed && exists) {
                        self.add_page_mutation(output, page);
                    }
                    if (page == last) break;
                }
            }
        }
    }

    [[nodiscard]] BackgroundEvaluationCallFrameSelection
    binding_selection() const
    {
        auto const& plan = graph_->background_evaluation_plan;
        BackgroundEvaluationCallFrameSelection selection{
            .binding_coverage =
                std::vector<Coverage>(plan.runtime.bindings.size()),
            .sample_rate = graph_->specialization.sample_rate,
        };
        for (auto const& required : prepared_->result().input_requirements) {
            auto const slot = plan.runtime.port_bindings[required.port];
            if (slot)
                selection.binding_coverage[*slot].include(required.required);
        }
        for (auto const& required : prepared_->result().output_requirements) {
            auto const slot = plan.runtime.port_bindings[required.port];
            if (slot && node_evaluates(plan.ports[required.port].node)) {
                selection.binding_coverage[*slot].include(required.required);
            }
        }
        return selection;
    }

    [[nodiscard]] BackgroundStorageSelection storage_selection(
        BackgroundEvaluationCallFrameSelection const& bindings) const
    {
        auto const& plan = graph_->background_evaluation_plan;
        BackgroundStorageSelection selection{
            .generation = graph_->project_generation,
            .storage_coverage =
                std::vector<Coverage>(plan.storage.ports.size()),
            .produce_storage =
                std::vector<bool>(plan.storage.ports.size(), false),
            .published = &pin_->snapshot(),
        };

        for (graph_jit::BackgroundBindingSlot slot = 0;
             slot < plan.runtime.bindings.size(); ++slot) {
            auto const& binding = plan.runtime.bindings[slot];
            auto const& coverage = bindings.binding_coverage[slot];
            if (coverage.empty()) continue;
            for (auto const storage_index : binding.storage) {
                bool direct = false;
                if (binding.kind == PortKind::sample &&
                    binding.direction == graph_jit::PortDirection::input) {
                    auto const& port = plan.ports[binding.port];
                    for (auto const& route : plan.storage.direct_samples) {
                        if (route.storage != storage_index ||
                            route.target_subset >=
                                plan.sample_target_subsets.size() ||
                            plan.sample_target_subsets[route.target_subset]
                                    .port != port.configured_port) {
                            continue;
                        }
                        selection.storage_coverage[storage_index].include(
                            shifted_back(coverage, route.read_latency));
                        direct = true;
                    }
                }
                if (!direct) {
                    selection.storage_coverage[storage_index].include(coverage);
                }
            }
        }

        // A valid retained output can satisfy a terminal request directly from
        // the pinned snapshot even when its producer is not scheduled.
        for (auto const& required : prepared_->result().output_requirements) {
            if (node_evaluates(plan.ports[required.port].node)) continue;
            for (graph_jit::PortStorageIndex index = 0;
                 index < plan.storage.ports.size(); ++index) {
                auto const& storage = plan.storage.ports[index];
                if (storage.storage ==
                        graph_jit::PortStorageKind::persisted_pages &&
                    storage.output_port == required.port) {
                    selection.storage_coverage[index].include(
                        required.required);
                }
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (auto const& materialization :
                 plan.storage.sample_materializations) {
                auto const target =
                    selection.storage_coverage[materialization.output];
                for (std::size_t source = 0;
                     source < materialization.inputs.size(); ++source) {
                    auto& selected =
                        selection
                            .storage_coverage[materialization.inputs[source]];
                    auto expanded =
                        selected |
                        shifted_back(
                            target,
                            materialization.source_read_latencies[source]);
                    if (expanded != selected) {
                        selected = std::move(expanded);
                        changed = true;
                    }
                }
            }
            for (auto const& materialization :
                 plan.storage.event_materializations) {
                auto const target =
                    selection.storage_coverage[materialization.output];
                for (auto const input : materialization.inputs) {
                    auto expanded = selection.storage_coverage[input] | target;
                    if (expanded != selection.storage_coverage[input]) {
                        selection.storage_coverage[input] = std::move(expanded);
                        changed = true;
                    }
                }
            }
        }

        for (graph_jit::PortStorageIndex index = 0;
             index < plan.storage.ports.size(); ++index) {
            auto const& storage = plan.storage.ports[index];
            if (!runtime_produced(plan, storage) ||
                selection.storage_coverage[index].empty()) {
                continue;
            }
            if (storage.sample_materialization ||
                storage.event_materialization) {
                selection.produce_storage[index] = true;
            } else if (storage.output_port) {
                selection.produce_storage[index] =
                    node_evaluates(plan.ports[*storage.output_port].node);
            }
        }
        return selection;
    }

    [[nodiscard]] std::expected<BackgroundEvaluationResult, std::string> run()
    {
        if (executed_) {
            return std::unexpected(
                "background evaluation transaction already executed");
        }
        executed_ = true;
        if (!graph_->background_operations.valid()) {
            return std::unexpected(
                "active graph has no complete background operations");
        }
        if (request_.page_width == 0) {
            return std::unexpected(
                "background evaluation page width must be nonzero");
        }

        reader_ = pages_->register_reader();
        pin_.emplace(reader_.pin());
        auto const& base = pin_->snapshot();
        if (request_.semantic_version < base.version().semantic) {
            return std::unexpected(
                "background evaluation semantic version cannot move backwards");
        }
        if (base.page_width() != 0 &&
            base.page_width() != request_.page_width &&
            (base.sample_page_count() != 0 || base.event_page_count() != 0)) {
            return std::unexpected(
                "persisted pages must be explicitly repaged before changing "
                "page width");
        }
        prepared_.emplace(
            propagation_->prepare(graph_->background_operations, node_storage_,
                                  *coverage_, request_.coverage,
                                  BackgroundCoverageDemandExpansion{
                                      .data = this,
                                      .expand_output = &expand_output_demand,
                                  }));

        auto binding = binding_selection();
        auto storage = storage_selection(binding);
        auto realization = std::make_unique<BackgroundStorageRealization>(
            graph_->background_evaluation_plan, std::move(storage));
        if (auto sealed = realization->seal(); !sealed) {
            return std::unexpected(std::move(sealed.error()));
        }

        BackgroundEvaluationCallFrame frame{graph_->background_evaluation_plan,
                                            *realization, binding};
        for (std::size_t node = 0; node < prepared_->node_activity().size();
             ++node) {
            frame.nodes()[node].activity = prepared_->node_activity()[node];
        }
        auto const& runtime = graph_->background_evaluation_plan.runtime;
        for (graph_jit::BackgroundReplayInvocationSlot slot = 0;
             slot < runtime.replay_invocations.size(); ++slot) {
            Coverage requested;
            auto const& replay = runtime.replay_invocations[slot];
            for (auto const binding_slot : replay.output_bindings) {
                requested.include(binding.binding_coverage[binding_slot]);
            }
            if (requested.empty()) {
                for (auto const binding_slot : replay.input_bindings) {
                    requested.include(binding.binding_coverage[binding_slot]);
                }
            }
            auto set = frame.set_replay_regions(
                slot, replay_regions(requested, replay.maximum_block_size));
            if (!set) return std::unexpected(std::move(set.error()));
        }
        if (auto sealed = frame.seal(); !sealed) {
            return std::unexpected(std::move(sealed.error()));
        }

        std::optional<PersistedPageStore::Candidate> candidate;
        auto const& storage_selection = realization->selection();
        bool has_produced_pages = false;
        for (graph_jit::PortStorageIndex index = 0;
             index < graph_->background_evaluation_plan.storage.ports.size();
             ++index) {
            auto const& planned =
                graph_->background_evaluation_plan.storage.ports[index];
            if (planned.storage ==
                    graph_jit::PortStorageKind::persisted_pages &&
                storage_selection.produce_storage[index] &&
                !storage_selection.storage_coverage[index].empty()) {
                has_produced_pages = true;
                break;
            }
        }
        if (has_produced_pages || !page_mutations_.empty()) {
            candidate.emplace(pages_->begin_candidate(
                request_.semantic_version, request_.page_width));
            if (candidate->base_version() != pin_->snapshot().version()) {
                discard_prepared();
                return BackgroundEvaluationResult{
                    .status = BackgroundEvaluationStatus::stale_base,
                };
            }
            for (auto const& mutation : page_mutations_) {
                candidate->erase_page(mutation.output, mutation.page);
            }
        }

        graph_->background_operations.evaluate(node_storage_, &frame.call());
        if (auto complete = frame.validate_evaluation(); !complete) {
            return std::unexpected(std::move(complete.error()));
        }

        std::optional<PersistedPageSnapshotVersion> published;
        if (candidate) {
            if (auto staged = realization->stage_persisted_pages(*candidate);
                !staged) {
                return std::unexpected(std::move(staged.error()));
            }
            published = candidate->target_version();
            if (pages_->publish(std::move(*candidate)) ==
                PersistedPagePublishResult::stale_base) {
                discard_prepared();
                return BackgroundEvaluationResult{
                    .status = BackgroundEvaluationStatus::stale_base,
                };
            }
        } else if (!pages_->is_current(pin_->snapshot())) {
            discard_prepared();
            return BackgroundEvaluationResult{
                .status = BackgroundEvaluationStatus::stale_base,
            };
        }

        auto result = propagation_->commit(*coverage_, std::move(*prepared_));
        prepared_.reset();
        return BackgroundEvaluationResult{
            .status = BackgroundEvaluationStatus::committed,
            .coverage = std::move(result),
            .published_pages = published,
        };
    }

public:
    Impl(CompiledGraph const& graph, std::byte* node_storage,
         BackgroundCoverageState& coverage,
         BackgroundPropagationWorkspace& propagation, PersistedPageStore& pages,
         BackgroundEvaluationRequest request)
        : graph_(&graph)
        , node_storage_(node_storage)
        , coverage_(&coverage)
        , propagation_(&propagation)
        , pages_(&pages)
        , request_(std::move(request))
    {}

    ~Impl()
    {
        discard_prepared();
        release_reader();
    }

    [[nodiscard]] std::expected<BackgroundEvaluationResult, std::string>
    execute()
    {
        try {
            auto result = run();
            if (!result) discard_prepared();
            release_reader();
            return result;
        } catch (std::exception const& error) {
            discard_prepared();
            release_reader();
            return std::unexpected(error.what());
        } catch (...) {
            discard_prepared();
            release_reader();
            return std::unexpected(
                "background evaluation failed with an unknown exception");
        }
    }
};

BackgroundEvaluationTransaction::BackgroundEvaluationTransaction(
    CompiledGraph const& graph, std::byte* node_storage,
    BackgroundCoverageState& coverage,
    BackgroundPropagationWorkspace& propagation, PersistedPageStore& pages,
    BackgroundEvaluationRequest request)
    : impl_(std::make_unique<Impl>(graph, node_storage, coverage, propagation,
                                   pages, std::move(request)))
{}

BackgroundEvaluationTransaction::~BackgroundEvaluationTransaction() = default;
BackgroundEvaluationTransaction::BackgroundEvaluationTransaction(
    BackgroundEvaluationTransaction&&) noexcept = default;
BackgroundEvaluationTransaction& BackgroundEvaluationTransaction::operator=(
    BackgroundEvaluationTransaction&&) noexcept = default;

std::expected<BackgroundEvaluationResult, std::string>
BackgroundEvaluationTransaction::execute()
{
    if (!impl_) {
        return std::unexpected(
            "background evaluation transaction has been moved from");
    }
    return impl_->execute();
}

} // namespace iv
