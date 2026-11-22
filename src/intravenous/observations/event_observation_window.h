#pragma once

#include <intravenous/execution/background/background_evaluation_call_frame.h>
#include <intravenous/execution/background/background_evaluation_transaction.h>
#include <intravenous/observations/observation_types.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace iv {

inline void project_recorded_event_timestamps(
    ObservationWindowQuery& query, std::span<TimedEvent const> timeline)
{
    auto& result = query.result;
    GlobalIndexRange{query.window.begin, query.window.length}.for_each_signed_order_segment([&](auto range) {
        auto const first = std::ranges::lower_bound(timeline, range.begin, SignedGlobalIndexOrder{}, &TimedEvent::time);
        for (auto it = first; it != timeline.end() && range.contains(it->time); ++it) {
            result.event_indices.push_back(it->time);
        }
    });
}

class BeatGridObservation {
    struct Search {
        std::uint64_t first = 0;
        std::uint64_t last = 0;
        std::uint64_t time_offset = 0;
    };
    struct IndexedWindow {
        graph_jit::BackgroundPortIndex output;
        std::size_t region;
        GlobalIndexRange range;
        std::uint64_t event_count;
        std::uint8_t depth;
        std::array<Search, 2> bounds;
    };
    struct IndexedSelection {
        IndexedWindow window;
        IndexRun run;
    };
    struct PositionalWindow {
        graph_jit::PublicEventCoverageInputView::Source source;
        GlobalIndexRange range;
        std::size_t region;
    };

    ObservationWindowQuery& query_;
    graph_jit::PublicEventCoverageInputView const& input_;
    graph_jit::BackgroundEvaluationPlan const& plan_;
    std::vector<PositionalWindow> positional_;
    std::vector<IndexedSelection> indexed_;
    std::size_t mark_budget_;

    static auto mark_order(TimedEvent const& event)
    {
        return std::pair{SignedGlobalIndexOrder::key(event.time), std::get<BeatEvent>(event.value).depth};
    }

    void project_indexed_runs()
    {
        auto& result = query_.result;
        auto& events = result.events;
        std::vector<std::uint8_t> retained;
        if (events.size() > mark_budget_) {
            // Preserve the plain observation's time and depth budget. The
            // retained events can split a run at the signed timeline seam.
            std::vector<std::size_t> ordered(events.size());
            std::iota(ordered.begin(), ordered.end(), std::size_t{0});
            std::ranges::partial_sort(ordered, ordered.begin() + mark_budget_, {}, [&](auto index) {
                return std::pair{mark_order(events[index]), index};
            });
            retained.resize(events.size());
            for (std::size_t index = 0; index < mark_budget_; ++index) retained[ordered[index]] = 1;
            result.truncated = true;
        }
        auto keep = [&](std::size_t index) { return retained.empty() || retained[index]; };
        std::size_t offset = 0;
        for (auto const& [window, selected] : indexed_) {
            ObservationBeatRun* current = nullptr;
            for (std::uint64_t slot = 0; slot < selected.count; ++slot, ++offset) {
                if (!keep(offset)) {
                    current = nullptr;
                    continue;
                }
                if (!current) current = &result.beat_runs.emplace_back(ObservationBeatRun{
                    .source = window.output, .region_index = window.region, .region = window.range,
                    .event_count = window.event_count, .depth = window.depth,
                    .indices = {selected.first + slot * selected.step, 0, selected.step},
                });
                current->samples.push_back(events[offset].time);
                ++current->indices.count;
            }
        }
        std::size_t count = 0;
        for (auto index = offset; index < events.size(); ++index)
            if (keep(index)) events[count++] = std::move(events[index]);
        events.resize(count);
    }

public:
    BeatGridObservation(ObservationWindowQuery& query, graph_jit::PublicEventCoverageInputView const& input,
        graph_jit::BackgroundEvaluationPlan const& plan, std::size_t mark_budget)
        : query_(query), input_(input), plan_(plan), mark_budget_(mark_budget)
    {}

    void select(BackgroundDemandSelection const& candidate, CoveragePropagationRequest& roots,
        std::size_t max_probes, std::size_t max_positional_events)
    {
        auto remaining_positional_events = max_positional_events;
        query_.result.truncated |= mark_budget_ < query_.window.points;
        GlobalIndexRange const viewport{query_.window.begin, query_.window.length};
        std::vector<IndexedWindow> windows;
        for (auto const& source : input_.sources) {
            auto const regions = candidate.event_regions(candidate.data, source.output);
            auto const published = candidate.output_regions(candidate.data, source.output);
            for (std::size_t index = 0; index < published.size(); ++index) {
                auto const range = published[index];
                auto const metadata = index < regions.size() ? regions[index].index : std::nullopt;
                auto const* beat = metadata && metadata->constant_value && source.conversion.step_count == 0
                    ? std::get_if<BeatEvent>(&*metadata->constant_value) : nullptr;
                if (beat && beat->depth > *query_.window.max_depth) continue;
                for_each_range_intersection(range, viewport, [&](GlobalIndexRange part) {
                    if (!beat) {
                        auto const& port = plan_.ports[source.output];
                        auto const rate_bound = std::ceil(static_cast<long double>(part.extent) * port.max_events_per_index);
                        auto const count = port.max_event_count
                            ? std::min(rate_bound, static_cast<long double>(*port.max_event_count)) : rate_bound;
                        if (!std::isfinite(count) || count > remaining_positional_events) {
                            query_.result.unavailable_regions.push_back(part);
                            return;
                        }
                        remaining_positional_events -= static_cast<std::size_t>(count);
                        positional_.push_back({source, part, index});
                        auto root = std::ranges::find(roots.output_demands, source.output, &OutputCoverageRequest::port);
                        if (root == roots.output_demands.end())
                            root = roots.output_demands.insert(root, {.port = source.output});
                        root->required.include(part);
                        return;
                    }
                    if (!metadata->event_count) return;
                    // Admission pessimistically accounts for both binary
                    // searches. Empty or complete sides require no probes.
                    if (windows.size() >= max_probes / 128) {
                        query_.result.unavailable_regions.push_back(part);
                        return;
                    }
                    auto const first = range.begin.forward_distance_to(part.begin);
                    auto const last = first + part.extent;
                    auto const count = metadata->event_count;
                    windows.push_back({source.output, index, range, count, beat->depth, {{
                        {0, first == 0 ? 0 : count, first},
                        {last == range.extent ? count : 0, count, last},
                    }}});
                });
            }
        }
        for (;;) {
            if (query_.canceled.load(std::memory_order_relaxed)) return;
            std::vector<IndexedRegionDemand> requests;
            std::vector<std::pair<IndexedWindow const*, Search*>> searches;
            for (auto& window : windows) {
                for (auto& bound : window.bounds) {
                    if (bound.first == bound.last) continue;
                    auto const middle = bound.first + (bound.last - bound.first) / 2;
                    requests.push_back({window.output, window.region, {{middle, 1}}});
                    searches.emplace_back(&window, &bound);
                }
            }
            if (requests.empty()) break;
            auto const values = candidate.read_events(candidate.data, requests);
            for (std::size_t i = 0; i < requests.size(); ++i) {
                auto const [window, bound] = searches[i];
                auto const middle = requests[i].runs.front().first;
                if (window->range.begin.forward_distance_to(values[i].front().time) < bound->time_offset)
                    bound->first = middle + 1;
                else
                    bound->last = middle;
            }
        }

        std::ranges::stable_sort(windows, {}, &IndexedWindow::depth);
        std::size_t remaining = mark_budget_;
        auto selection = [](IndexedWindow const& window, std::uint64_t stride) {
            auto const begin = window.bounds[0].first;
            auto const end = window.bounds[1].first;
            if (begin >= end) return IndexRun{};
            auto const offset = (stride - begin % stride) % stride;
            if (offset >= end - begin) return IndexRun{};
            auto const first = begin + offset;
            return IndexRun{first, 1 + (end - 1 - first) / stride, stride};
        };
        for (std::size_t first = 0; first < windows.size();) {
            auto last = first + 1;
            while (last < windows.size() && windows[last].depth == windows[first].depth) ++last;
            auto const level = std::span{windows}.subspan(first, last - first);
            auto fits = [&](std::uint64_t stride) {
                auto count = remaining;
                for (auto const& window : level) {
                    auto const run = selection(window, stride);
                    if (run.count > count) return false;
                    count -= static_cast<std::size_t>(run.count);
                }
                return true;
            };
            std::uint64_t stride = 1;
            if (remaining)
                while (!fits(stride) && stride < (std::uint64_t{1} << 63)) stride *= 2;
            if (!fits(stride)) {
                query_.result.truncated = true;
                if (first == 0) query_.result.unavailable_regions.push_back(viewport);
                break;
            }
            for (auto const& window : level) {
                auto const run = selection(window, stride);
                if (!run.count) continue;
                indexed_.push_back({window, run});
                roots.indexed_output_demands.push_back({window.output, window.region, {run}});
                remaining -= static_cast<std::size_t>(run.count);
            }
            if (stride > 1) {
                query_.result.truncated = true;
                break;
            }
            first = last;
        }
    }

    void project(BackgroundStorageRealization const& storage, BackgroundEvaluationCallFrame const& frame,
        PersistedDataVersion version)
    {
        auto& result = query_.result;
        result.semantic_version = version.semantic;
        result.data_version = version.revision;
        auto add = [&](TimedEvent const& event) {
            auto const* beat = std::get_if<BeatEvent>(&event.value);
            if (beat && beat->depth <= *query_.window.max_depth) result.events.push_back(event);
        };
        for (auto const& [window, selected] : indexed_) {
            auto const runs = frame.indexed_events(window.output);
            for (std::uint64_t slot = 0; slot < selected.count; ++slot) {
                auto const index = selected.first + slot * selected.step;
                auto const* event = details::read_prepared_event(runs, window.region, index);
                if (!event) throw std::logic_error("beat-grid occurrence was not prepared");
                result.events.push_back(*event);
            }
        }
        for (auto const& window : positional_) {
            BackgroundEventReadView const* selected = nullptr;
            std::optional<std::size_t> selected_region;
            for (std::size_t index = 0; index < plan_.storage.ports.size(); ++index) {
                auto const& slot = plan_.storage.ports[index];
                if (slot.output_port != window.source.output) continue;
                auto const* view = storage.event_read(index);
                if (!view || !view->coverage().contains(window.range)) continue;
                if (plan_.ports[window.source.output].authored_tock_output) {
                    selected_region = storage.prepared_region_index(index, window.region);
                    if (!selected_region) continue;
                }
                selected = view;
                break;
            }
            if (!selected) {
                result.unavailable_regions.push_back(window.range);
                continue;
            }
            auto receive = [&](TimedEvent const& event) {
                EventConversionRegistry::convert(window.source.conversion, event, add);
            };
            if (selected_region) selected->for_each_region(*selected_region, window.range, receive);
            else selected->for_each(window.range, receive);
        }
        if (query_.window.include_indexed_runs && !indexed_.empty()) project_indexed_runs();
        std::ranges::sort(result.events, {}, mark_order);
        if (result.events.size() > mark_budget_) {
            result.events.resize(mark_budget_);
            result.truncated = true;
        }
    }
};
} // namespace iv
