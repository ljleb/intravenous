#pragma once

#include <intravenous/lane_node/generate.h>

#include <vector>

namespace iv {

struct BorrowedSampleBlock {
    std::span<Sample const> samples {};
    ChannelLayout channel_layout {
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };
    size_t frame_count = 0;

    [[nodiscard]] bool empty() const
    {
        return frame_count == 0 || samples.empty();
    }

    [[nodiscard]] SampleBlockView<Sample const> view() const
    {
        return SampleBlockView<Sample const>(samples, channel_layout, frame_count);
    }
};

struct OwnedSampleBlock {
    std::vector<Sample> samples {};
    ChannelLayout channel_layout {
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };
    size_t frame_count = 0;

    [[nodiscard]] bool empty() const
    {
        return frame_count == 0 || samples.empty();
    }

    [[nodiscard]] SampleBlockView<Sample const> view() const
    {
        return SampleBlockView<Sample const>(samples, channel_layout, frame_count);
    }
};

struct SampleStorageBlock {
    std::vector<Sample::storage> samples {};
    ChannelLayout channel_layout {
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };
    size_t frame_count = 0;
};

inline OwnedSampleBlock copy_sample_block(SampleBlockView<Sample const> view)
{
    return OwnedSampleBlock{
        .samples = std::vector<Sample>(view.samples().begin(), view.samples().end()),
        .channel_layout = view.channel_layout(),
        .frame_count = view.frames(),
    };
}

inline SampleStorageBlock copy_sample_storage_block(
    SampleBlockView<Sample const> view,
    ChannelLayout target_layout)
{
    std::vector<Sample> converted;
    SampleBlockView<Sample const> source = view;
    if (source.channel_layout() != target_layout) {
        converted.assign(sample_storage_size(target_layout, source.frames()), Sample {});
        auto const plan = ChannelConversionRegistry::plan(source.channel_layout(), target_layout);
        plan.convert(source.samples().data(), converted.data(), source.frames());
        source = SampleBlockView<Sample const>(converted, target_layout, source.frames());
    }

    std::vector<Sample::storage> result(source.samples().size());
    for (size_t i = 0; i < source.samples().size(); ++i) {
        result[i] = source.samples()[i].value;
    }
    return SampleStorageBlock{
        .samples = std::move(result),
        .channel_layout = source.channel_layout(),
        .frame_count = source.frames(),
    };
}

inline SampleStorageBlock copy_sample_storage_block_planar(
    SampleBlockView<Sample const> view)
{
    return copy_sample_storage_block(
        view,
        ChannelLayout{
            .channel_type = view.channel_type(),
            .sample_layout = SampleStreamLayout::planar,
        });
}

} // namespace iv
