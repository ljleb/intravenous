#include <intravenous/runtime/sample_stream_blocks.h>

#include <gtest/gtest.h>

namespace {

iv::BorrowedSampleBlock borrowed_block(
    std::span<iv::Sample const> samples,
    iv::ChannelLayout layout,
    size_t frame_count)
{
    return iv::BorrowedSampleBlock{
        .samples = samples,
        .channel_layout = layout,
        .frame_count = frame_count,
    };
}

std::vector<iv::Sample> sample_values(iv::SampleStorageBlock const& block)
{
    std::vector<iv::Sample> values;
    values.reserve(block.samples.size());
    for (auto const sample : block.samples) {
        values.push_back(iv::Sample{sample});
    }
    return values;
}

}

TEST(Channels, MonoPlanarIdentityConversionPreservesExactSamples)
{
    auto const samples = std::array<iv::Sample, 5>{
        iv::Sample{1.0f},
        iv::Sample{-2.0f},
        iv::Sample{3.5f},
        iv::Sample{0.25f},
        iv::Sample{-0.75f},
    };

    auto const converted = iv::copy_sample_storage_block(
        borrowed_block(
            samples,
            iv::ChannelLayout{
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            samples.size())
            .view(),
        iv::ChannelLayout{
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        });

    EXPECT_EQ(converted.channel_layout.channel_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(converted.channel_layout.sample_layout, iv::SampleStreamLayout::planar);
    EXPECT_EQ(converted.frame_count, samples.size());
    EXPECT_EQ(sample_values(converted), (std::vector<iv::Sample>{
        iv::Sample{1.0f},
        iv::Sample{-2.0f},
        iv::Sample{3.5f},
        iv::Sample{0.25f},
        iv::Sample{-0.75f},
    }));
}

TEST(Channels, StereoInterleavedIdentityConversionPreservesExactSamples)
{
    auto const samples = std::array<iv::Sample, 8>{
        iv::Sample{1.0f},  iv::Sample{10.0f},
        iv::Sample{2.0f},  iv::Sample{20.0f},
        iv::Sample{3.0f},  iv::Sample{30.0f},
        iv::Sample{4.0f},  iv::Sample{40.0f},
    };

    auto const converted = iv::copy_sample_storage_block(
        borrowed_block(
            samples,
            iv::ChannelLayout{
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
            4)
            .view(),
        iv::ChannelLayout{
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        });

    EXPECT_EQ(converted.channel_layout.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(converted.channel_layout.sample_layout, iv::SampleStreamLayout::interleaved);
    EXPECT_EQ(converted.frame_count, 4u);
    EXPECT_EQ(sample_values(converted), (std::vector<iv::Sample>{
        iv::Sample{1.0f},  iv::Sample{10.0f},
        iv::Sample{2.0f},  iv::Sample{20.0f},
        iv::Sample{3.0f},  iv::Sample{30.0f},
        iv::Sample{4.0f},  iv::Sample{40.0f},
    }));
}

TEST(Channels, ZeroFrameConversionProducesEmptyStorage)
{
    auto const converted = iv::copy_sample_storage_block(
        borrowed_block(
            std::span<iv::Sample const>{},
            iv::ChannelLayout{
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            0)
            .view(),
        iv::ChannelLayout{
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        });

    EXPECT_TRUE(converted.samples.empty());
    EXPECT_EQ(converted.frame_count, 0u);
    EXPECT_EQ(converted.channel_layout.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(converted.channel_layout.sample_layout, iv::SampleStreamLayout::interleaved);
}

TEST(Channels, OddFrameMonoToStereoInterleavedConversionBroadcastsEveryFrame)
{
    auto const samples = std::array<iv::Sample, 5>{
        iv::Sample{1.0f},
        iv::Sample{-2.0f},
        iv::Sample{3.5f},
        iv::Sample{0.25f},
        iv::Sample{-0.75f},
    };

    auto const converted = iv::copy_sample_storage_block(
        borrowed_block(
            samples,
            iv::ChannelLayout{
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            samples.size())
            .view(),
        iv::ChannelLayout{
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        });

    EXPECT_EQ(converted.frame_count, samples.size());
    EXPECT_EQ(sample_values(converted), (std::vector<iv::Sample>{
        iv::Sample{1.0f}, iv::Sample{1.0f},
        iv::Sample{-2.0f}, iv::Sample{-2.0f},
        iv::Sample{3.5f}, iv::Sample{3.5f},
        iv::Sample{0.25f}, iv::Sample{0.25f},
        iv::Sample{-0.75f}, iv::Sample{-0.75f},
    }));
}
