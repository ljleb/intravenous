#include <intravenous/execution/storage/persisted_data_store.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace iv;
using Clock = std::chrono::steady_clock;
std::size_t process_maps()
{
    std::ifstream maps{"/proc/self/maps"};
    std::size_t count = 0;
    std::string line;
    while (std::getline(maps, line)) ++count;
    return count;
}
enum class PatchPattern { dense, scattered, same_page, cross_page };
PersistedSampleRegion payload(PersistedOutputHandle output, GlobalIndexRange range, float value, ChannelLayout layout)
{
    PersistedSampleRegion result{.output = output, .range = range, .layout = layout};
    result.values.assign(static_cast<std::size_t>(range.extent) * channel_count(layout), Sample{value});
    return result;
}

void measure_cache(std::size_t pages, std::size_t iterations, std::size_t pinned_versions)
{
    PersistedDataStore probe;
    auto const page = probe.buffered_storage_usage().page_size;
    auto const bytes = pages * page;
    BufferedStorageLimits limits;
    limits.max_cached_payload_bytes = 3 * bytes;
    limits.max_backing_bytes = (5 + pinned_versions) * bytes;
    PersistedDataStore store(limits);
    auto handle = [&](std::size_t port) {
        return store.resolve_output(GenerationLocalPersistedOutputId{1, port, PortKind::sample});
    };
    auto const recording = handle(0);
    GlobalIndexRange whole{GlobalIndex::from_signed(0), bytes / sizeof(Sample)};
    auto seed = store.begin_candidate(1);
    seed.overwrite(payload(recording, whole, 42, mono_interleaved_channel_layout));
    if (store.publish(std::move(seed)) != PersistedDataPublishResult::published)
        throw std::runtime_error("cache benchmark recording publication failed");
    (void)store.reclaim_retired();
    std::vector<PersistedDataStore::ReaderSlot> readers;
    std::vector<PersistedDataStore::ReaderPin> pins(pinned_versions);
    std::vector<PersistedOutputHandle> held(pinned_versions);
    std::vector<float> held_values(pinned_versions);
    for (std::size_t index = 0; index < pinned_versions; ++index) readers.push_back(store.register_reader());
    std::vector<double> elapsed;
    std::size_t peak_live = bytes;
    for (std::size_t index = 0; index < iterations; ++index) {
        if (pinned_versions) pins[index % pinned_versions] = {};
        auto output = handle(1 + index % 12);
        auto const realization = index % 2 ? OutputRealization::buffered : OutputRealization::computed;
        store.set_cache_realization(output, realization);
        auto const started = Clock::now();
        store.prepare_cache(bytes, std::array{PersistedPayloadSelection{output, IndexCoverage{whole}}}, [](auto) {});
        auto candidate = store.begin_candidate(index + 2);
        auto region = payload(output, whole, static_cast<float>(index), mono_planar_channel_layout);
        candidate.replace_sample_regions(output, region.layout, region.geometry, IndexCoverage{whole},
            std::array{whole}, {{.region_index = 0, .payload = std::move(region)}});
        if (store.publish(std::move(candidate)) != PersistedDataPublishResult::published)
            throw std::runtime_error("cache benchmark publication became stale");
        (void)store.reclaim_retired();
        elapsed.push_back(std::chrono::duration<double, std::micro>(Clock::now() - started).count());
        peak_live = std::max(peak_live, store.buffered_storage_usage().live_page_bytes);
        if (pinned_versions) {
            auto slot = index % pinned_versions;
            pins[slot] = readers[slot].pin();
            pins[slot].select_outputs(std::array{output});
            pins[slot].retain_payload(std::array{PersistedPayloadSelection{output, IndexCoverage{whole}}});
            held[slot] = output;
            held_values[slot] = static_cast<float>(index);
            for (std::size_t selected = 0; selected < pinned_versions; ++selected) {
                if (selected > index) break;
                auto region = pins[selected]->find_sample_region(held[selected], whole.begin);
                if (!region || region->at(whole.begin, 0).value != held_values[selected])
                    throw std::runtime_error("cache eviction changed a pinned reader");
            }
        }
    }
    pins.clear();
    (void)store.reclaim_retired();
    store.prepare_cache(0, {}, [](auto) {});
    auto reader = store.register_reader();
    auto pin = reader.pin();
    if (pin->find_sample_region(recording, whole.begin)->at(whole.begin, 0).value != 42)
        throw std::runtime_error("cache pressure changed authoritative recording data");
    auto usage = store.buffered_storage_usage();
    if (usage.backing_bytes > limits.max_backing_bytes || usage.live_page_bytes < bytes)
        throw std::runtime_error("cache benchmark violated its backing budget");
    std::ranges::sort(elapsed);
    std::cout << "case\tpages\titerations\treaders\tmedian_us\tp95_us\tpeak_live_bytes\tlive_bytes\tbacking_bytes\tcache_evictions\treused_pages\tmapping_calls\n"
        << "cache_pressure\t" << pages << '\t' << iterations << '\t' << pinned_versions << '\t'
        << elapsed[elapsed.size() / 2] << '\t' << elapsed[(elapsed.size() - 1) * 95 / 100] << '\t'
        << peak_live << '\t' << usage.live_page_bytes << '\t' << usage.backing_bytes << '\t'
        << usage.cache_evictions << '\t' << usage.reused_pages << '\t' << usage.mapping_calls << '\n';
}
void measure(std::string const& name, std::size_t pages, std::size_t iterations, std::size_t pinned_versions,
    BufferedStorageLimits limits, bool tock, PatchPattern pattern, bool stereo = false)
{
    PersistedDataStore store{limits};
    auto const page_size = store.buffered_storage_usage().page_size;
    auto const per_page = page_size / sizeof(Sample);
    ChannelLayout const layout{.channel_type = stereo ? ChannelTypeId::stereo : ChannelTypeId::mono,
        .sample_layout = tock ? SampleStreamLayout::planar : SampleStreamLayout::interleaved};
    auto const channels = channel_count(layout);
    auto const payload_bytes = pages * page_size * channels;
    GlobalIndexRange whole{GlobalIndex::from_signed(0), pages * per_page};
    auto const output = store.resolve_output(GenerationLocalPersistedOutputId{.generation = 1, .port = 0, .kind = PortKind::sample});
    auto changed_range = [&](std::size_t index) {
        if (pattern == PatchPattern::dense) return whole;
        std::size_t offset = 0, count = 1;
        if (pattern == PatchPattern::scattered) offset = ((index * 37) % pages) * per_page;
        else if (pattern == PatchPattern::same_page) offset = (index * 37) % per_page;
        else { offset = ((index * 37) % (pages - 1)) * per_page + per_page - 1; count = 2; }
        return GlobalIndexRange{whole.begin.advanced_by(offset), count};
    };
    auto write = [&](PersistedDataStore::Candidate& candidate, GlobalIndexRange range, float value) {
        if (!tock) { candidate.overwrite(payload(output, range, value, layout)); return; }
        PersistedSampleRegion region{.output = output, .range = whole, .layout = layout};
        auto const window_frames = std::min<std::size_t>(limits.max_contiguous_mapping_bytes, 1024 * 1024) / (channels * sizeof(Sample));
        std::vector<PersistedSampleRegion> windows;
        for (std::uint64_t offset = 0; offset < range.extent;) {
            GlobalIndexRange part{range.begin.advanced_by(offset), std::min<std::uint64_t>(range.extent - offset, window_frames)};
            windows.push_back(payload(output, part, value, layout));
            offset += part.extent;
        }
        region.set_prepared_windows(std::move(windows));
        candidate.replace_sample_regions(output, region.layout, region.geometry,
            IndexCoverage{range}, std::vector{whole}, {{.region_index = 0, .payload = std::move(region)}});
    };
    auto seed = store.begin_candidate(1);
    write(seed, whole, 0);
    if (store.publish(std::move(seed)) != PersistedDataPublishResult::published) throw std::runtime_error("seed publication failed");
    std::vector<PersistedDataStore::ReaderSlot> readers;
    std::vector<PersistedDataStore::ReaderPin> pins(pinned_versions);
    for (std::size_t index = 0; index < pinned_versions; ++index) readers.push_back(store.register_reader());
    std::vector<double> elapsed;
    elapsed.reserve(iterations);
    auto peak_live = std::size_t{}, peak_reserved = std::size_t{}, peak_runs = std::size_t{};
    auto peak_process_maps = process_maps();
    auto const before = store.buffered_storage_usage();
    auto sample_usage = [&] {
        auto const usage = store.buffered_storage_usage();
        peak_live = std::max(peak_live, usage.live_page_bytes);
        peak_reserved = std::max(peak_reserved, usage.reserved_mapping_bytes);
        peak_runs = std::max(peak_runs, usage.mapped_runs);
    };
    for (std::size_t index = 0; index < iterations; ++index) {
        if (pinned_versions) {
            auto const slot = index % pinned_versions;
            pins[slot] = {};
            pins[slot] = readers[slot].pin();
        }
        auto const started = Clock::now();
        auto candidate = store.begin_candidate(index + 2);
        auto const changed = changed_range(index);
        write(candidate, changed, static_cast<float>(index + 1));
        sample_usage();
        if (store.publish(std::move(candidate)) != PersistedDataPublishResult::published) throw std::runtime_error("publication became stale");
        (void)store.reclaim_retired();
        elapsed.push_back(std::chrono::duration<double, std::micro>(Clock::now() - started).count());
        sample_usage();
        if (index % 16 == 0) peak_process_maps = std::max(peak_process_maps, process_maps());
    }
    pins.clear();
    (void)store.reclaim_retired();
    {
        auto reader = store.register_reader();
        auto pin = reader.pin();
        auto const last = changed_range(iterations - 1).begin;
        auto const* region = pin->find_sample_region(output, last);
        for (std::size_t channel = 0; channel < channels; ++channel)
            if (!region || region->at(last, channel).value != static_cast<float>(iterations))
                throw std::runtime_error("benchmark lost the final published value");
    }
    auto const after = store.buffered_storage_usage();
    if (after.live_page_bytes != payload_bytes)
        throw std::runtime_error("benchmark retained excess live pages after reader retirement: " + name);
    std::ranges::sort(elapsed);
    std::cout << name << '\t' << pages << '\t' << iterations << '\t' << pinned_versions << '\t'
        << limits.max_contiguous_mapping_bytes << '\t' << channels << '\t' << payload_bytes << '\t'
        << elapsed[elapsed.size() / 2] << '\t' << elapsed[(elapsed.size() - 1) * 95 / 100] << '\t'
        << after.backing_bytes << '\t' << after.file_extent_bytes << '\t' << after.live_page_bytes << '\t'
        << after.reusable_page_bytes << '\t' << after.cached_page_bytes << '\t'
        << peak_live << '\t' << peak_reserved << '\t' << peak_runs << '\t' << peak_process_maps << '\t'
        << after.reused_pages - before.reused_pages << '\t' << after.mapping_calls - before.mapping_calls << '\n';
}
}
int main(int argc, char** argv)
{
    try {
        if (argc > 1 && std::string_view{argv[1]} == "--cache") {
            auto pages = argc > 2 ? std::stoull(argv[2]) : 64;
            auto iterations = argc > 3 ? std::stoull(argv[3]) : 100;
            auto pins = argc > 4 ? std::stoull(argv[4]) : 4;
            if (argc > 5 || !pages || pages > 4096 || !iterations || iterations > 100000 || pins > 32)
                throw std::invalid_argument("usage: iv_buffered_storage_benchmark --cache [pages=64] [iterations=100] [pinned_readers=4]");
            measure_cache(pages, iterations, pins);
            return 0;
        }
        auto pages = argc > 1 ? std::stoull(argv[1]) : 64;
        auto iterations = argc > 2 ? std::stoull(argv[2]) : 200;
        auto pins = argc > 3 ? std::stoull(argv[3]) : 4;
        if (!pages || pages > 4096 || !iterations || iterations > 100000 || pins > 32 || argc > 5)
            throw std::invalid_argument("usage: iv_buffered_storage_benchmark [pages=64] [iterations=200] [pinned_versions=4] [mapping_limit_KiB=1048576]");
        BufferedStorageLimits limits;
        if (argc > 4) {
            auto const kib = std::stoull(argv[4]);
            if (!kib || kib > 4 * 1024 * 1024) throw std::invalid_argument("mapping limit must be 1..4194304 KiB");
            limits.max_contiguous_mapping_bytes = kib * 1024;
        }
        std::cout << "# Background writes/publication/reclamation; no LLVM or audio callback. Limits are provisional.\n"
            << "case\tpages_per_channel\titerations\tpinned_versions\tmapping_limit_bytes\tchannels\tpayload_bytes\tmedian_us\tp95_us\tbacking_bytes\tfile_extent_bytes\tlive_bytes\treusable_bytes\tcached_bytes\tpeak_live_bytes\tpeak_mapped_bytes\tpeak_runs\tpeak_process_vmas\treused_pages\tmapping_calls\n";
        for (bool tock : {false, true}) for (bool fragmented : {false, true})
            measure(std::string{tock ? "tock" : "recording"} + (fragmented ? "_fragmented" : "_dense"),
                pages, iterations, pins, limits, tock, fragmented ? PatchPattern::scattered : PatchPattern::dense);
        measure("tock_same_page", pages, iterations, pins, limits, true, PatchPattern::same_page);
        if (pages > 1) measure("tock_cross_page", pages, iterations, pins, limits, true, PatchPattern::cross_page);
        for (auto pattern : {PatchPattern::dense, PatchPattern::scattered, PatchPattern::same_page, PatchPattern::cross_page}) {
            if (pattern == PatchPattern::cross_page && pages < 2) continue;
            auto const suffix = pattern == PatchPattern::dense ? "dense" : pattern == PatchPattern::scattered ? "fragmented"
                : pattern == PatchPattern::same_page ? "same_page" : "cross_page";
            measure(std::string{"tock_planar_"} + suffix, pages, iterations, pins, limits, true, pattern, true);
        }
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
