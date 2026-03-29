#include "devices/channel_buffer_sink.h"
#include "basic_nodes/buffers.h"
#include "graph_node.h"
#include "module_test_utils.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <vector>

int main()
{
    iv::test::install_crash_handlers();

    {
        iv::Sample a = 0.25f;
        iv::Sample b = 0.5f;
        std::array<iv::Sample, 8> channel {};
        iv::AudioDevice audio_device(
            iv::RenderConfig{
                .sample_rate = 48000,
                .num_channels = 1,
                .max_block_frames = channel.size(),
            },
            false
        );

        iv::GraphBuilder g;
        auto const src_a = g.node<iv::ValueSource>(&a);
        auto const src_b = g.node<iv::ValueSource>(&b);
        auto const sink_a = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
            .device_id = 0,
            .channel = 0,
        });
        auto const sink_b = g.node<iv::AudioDeviceSink>(iv::AudioDeviceSink{
            .device_id = 0,
            .channel = 0,
        });
        sink_a(src_a);
        sink_b(src_b);
        g.outputs();

        iv::NodeExecutor executor = iv::NodeExecutor::create(
            iv::TypeErasedNode(g.build()),
            {},
            iv::ExecutionTargets(iv::test::make_audio_device_provider(audio_device))
        );
        executor.tick_block(0, channel.size());

        auto block = executor.execution_targets().audio_device(0, 0).output_block();
        std::copy_n(block.begin(), channel.size(), channel.begin());

        for (iv::Sample sample : channel) {
            if (sample != 0.75f) {
                std::cerr << "sink accumulation failed\n";
                return 1;
            }
        }
    }

    {
        iv::AudioDevice audio_device({}, false);
        auto loader = iv::test::make_loader();
        auto processor = iv::test::make_executor(
            loader,
            audio_device,
            iv::test::test_modules_root() / "noisy_saw_project"
        );
        iv::test::require(processor.num_module_refs() != 0, "module refs should be retained by NodeExecutor");
        iv::test::run_processor_ticks(processor);
        iv::test::require(
            !processor.execution_targets().audio_device(0, 0).is_shutdown_requested(),
            "directory module requested shutdown unexpectedly"
        );
    }

    return 0;
}
