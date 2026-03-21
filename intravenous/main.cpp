#include "devices/audio.h"
#include "basic_nodes.h"
#include "node.h"
#include "dsl.h"
#include "graph_node.h"
#include "wav.h"
#include <stacktrace>
#include <iostream>
#include <string>

using namespace iv;

[[noreturn]] void terminate_stacktrace() {
    std::cerr << "\nstd::terminate called\n";

    if (auto ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            std::cerr << std::stacktrace::current() << '\n';
            std::cerr << "uncaught exception: " << e.what() << '\n';
        } catch (...) {
            std::cerr << "uncaught non-std exception\n";
        }
    } else {
        std::cerr << "no active exception\n";
    }

    std::abort();
}

static void feedback_voice(GraphBuilder& g) {
    auto const amplitude = g.input("amplitude", 0.5);
    auto const frequency = g.input("frequency", 1000.0);
    auto const voice_noise = g.input("noise");
    auto const dt = g.input("dt", 1.0);
    auto const reset = g.input("reset", 1.0);

    auto const integrator = g.node<Integrator>();
    auto const warper = g.node<Warper>();

    integrator(warper["aliased"].detach() * reset, frequency * 2.0, dt);
    warper(integrator + voice_noise);
    g.outputs(warper["anti_aliased"] * amplitude);
}

static void noise_voice(GraphBuilder& g) {
    auto const dt = g.input("dt", 1.0);

    auto const level_knob = 0.5;
    auto const lo_pass_knob = 1.0;
    auto const hi_pass_knob = 1.0;
    auto const generator = g.node<DeterministicUniformAESNoise>();
    auto const lo_pass = g.node<SimpleIirLowPass>();
    auto const hi_pass = g.node<SimpleIirHighPass>();

    auto const u_to_n_knob = 0.0;
    auto const u_to_n = g.node<UniformToGaussian>(0.0, 0.5);
    auto const u_to_c = g.node<UniformToCauchy>(0.0, 0.01);
    auto const interp = g.node<Interpolation>();

    generator({ {"min", -1}, {"max", 1} });

    u_to_c(generator);
    u_to_n(generator);

    interp(u_to_n, u_to_c, u_to_n_knob);

    lo_pass(interp, lo_pass_knob, dt);
    hi_pass(lo_pass, hi_pass_knob, dt);

    g.outputs(hi_pass * level_knob);
}

NodeProcessor init_graph(
    Sample& sample_period,
    RealtimeOutputTarget& output_target,
    size_t num_channels
) {
    GraphBuilder g;

    auto const dt = g.node<ValueSource>(&sample_period);
    SignalRef first_noise;

    for (size_t i = 0; i < num_channels; ++i)
    {
        auto const noise = g.subgraph(noise_voice);
        if (i == 0) first_noise = noise;
        auto const voice = g.subgraph(feedback_voice);
        auto const out = g.node<OutputDeviceSink>(output_target, i);
        auto const shared_noise = g.node<Interpolation>();

        noise(dt);
        shared_noise(first_noise, noise, 1.0);
        voice(0.5f, 200.0f, shared_noise, dt);
        out(voice);
    }

    g.outputs();
    return NodeProcessor(std::move(g).build());
}

class AudioApp {
public:
    static constexpr size_t kMaxChannels = 2;
    static constexpr size_t kMaxCallbackFrames = 4096;

    AudioApp(): _impl(std::make_unique<Impl>()) {}
    ~AudioApp() {
        shutdown();
    }

    AudioApp(AudioApp const&) = delete;
    AudioApp& operator=(AudioApp const&) = delete;

    bool init() {
        return _impl->init();
    }
    void shutdown() {
        if (_impl) {
            _impl->shutdown();
        }
    }

private:
    struct Impl {
        ma_context context {};
        ma_device device {};

        bool context_initialized = false;
        bool device_initialized = false;
        bool device_started = false;

        Sample sample_period = 1.0f / 48000.0f;
        size_t global_index = 0;
        size_t num_channels = 2;

        RealtimeOutputTarget output_target {};
        std::unique_ptr<NodeProcessor> processor;

        std::array<std::array<Sample, AudioApp::kMaxCallbackFrames>, AudioApp::kMaxChannels> planar_storage {};
        std::array<Sample*, AudioApp::kMaxChannels> channel_ptrs {};

        Impl() {
            for (size_t ch = 0; ch < AudioApp::kMaxChannels; ++ch) {
                channel_ptrs[ch] = planar_storage[ch].data();
            }
        }

        void clear_staging(size_t channels, size_t frames) {
            for (size_t ch = 0; ch < channels; ++ch) {
                std::fill_n(planar_storage[ch].data(), frames, 0.0f);
            }
        }

        static void data_callback(ma_device* device, void* output, void const*, ma_uint32 frame_count) {
            auto* self = static_cast<Impl*>(device->pUserData);
            auto* out = static_cast<float*>(output);

            if (!self || !self->processor) {
                std::memset(out, 0, static_cast<size_t>(frame_count) * device->playback.channels * sizeof(float));
                return;
            }

            if (device->playback.channels > AudioApp::kMaxChannels || frame_count > AudioApp::kMaxCallbackFrames) {
                std::memset(out, 0, static_cast<size_t>(frame_count) * device->playback.channels * sizeof(float));
                return;
            }

            self->clear_staging(device->playback.channels, frame_count);

            self->output_target.begin(
                self->channel_ptrs.data(),
                device->playback.channels,
                frame_count,
                self->global_index
            );

            for (ma_uint32 i = 0; i < frame_count; ++i) {
                self->processor->tick({}, self->global_index + i);
            }

            self->output_target.end();

            for (ma_uint32 i = 0; i < frame_count; ++i) {
                for (ma_uint32 ch = 0; ch < device->playback.channels; ++ch) {
                    out[static_cast<size_t>(i) * device->playback.channels + ch] =
                        self->planar_storage[ch][i];
                }
            }

            self->global_index += frame_count;
        }

        bool init() {
            ma_result result = ma_context_init(nullptr, 0, nullptr, &context);
            if (result != MA_SUCCESS) {
                std::cerr << "ma_context_init failed\n";
                return false;
            }
            context_initialized = true;

            ma_device_config config = ma_device_config_init(ma_device_type_playback);
            config.playback.format = ma_format_f32;
            config.playback.channels = 2;
            config.sampleRate = 48000;
            config.dataCallback = data_callback;
            config.pUserData = this;

            result = ma_device_init(&context, &config, &device);
            if (result != MA_SUCCESS) {
                std::cerr << "ma_device_init failed\n";
                shutdown();
                return false;
            }
            device_initialized = true;

            num_channels = device.playback.channels;
            sample_period = 1.0f / static_cast<Sample>(device.sampleRate);

            processor = std::make_unique<NodeProcessor>(
                init_graph(sample_period, output_target, num_channels)
            );

            result = ma_device_start(&device);
            if (result != MA_SUCCESS) {
                std::cerr << "ma_device_start failed\n";
                shutdown();
                return false;
            }
            device_started = true;

            return true;
        }

        void shutdown() {
            processor.reset();

            if (device_initialized) {
                ma_device_uninit(&device);
                device_initialized = false;
                device_started = false;
            }

            if (context_initialized) {
                ma_context_uninit(&context);
                context_initialized = false;
            }
        }
    };
    std::unique_ptr<Impl> _impl;
};

int main() {
#ifndef NDEBUG
    std::set_terminate(terminate_stacktrace);
#endif

    AudioApp app;
    if (!app.init()) {
        std::cerr << "failed to initialize audio\n";
        return 1;
    }

    std::cout << "Audio running. Press Enter to quit.\n";
    std::string line;
    std::getline(std::cin, line);

    app.shutdown();
    return 0;
}
