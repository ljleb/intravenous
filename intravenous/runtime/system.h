#pragma once
#include "devices/audio_device.h"
#include "juce_vst_runtime.h"
#include "wav.h"
#include <cstdlib>
#include <iostream>
#include <thread>

namespace iv {
    inline char const* file_render_path_env()
    {
        char const* value = std::getenv("IV_FILE_RENDER_PATH");
        return (value && *value) ? value : nullptr;
    }

    struct SystemProcessorRuntime {
        AudioRenderSession render_session;
#if IV_ENABLE_JUCE_VST
        JuceVstRuntimeSupport juce_vst_runtime;
#endif

        struct SinkBinding {
            size_t channel = 0;
            std::span<Sample> buffer;
        };

        std::vector<SinkBinding> sink_bindings;

        explicit SystemProcessorRuntime(
            AudioDevice& audio_device
#if IV_ENABLE_JUCE_VST
            , JuceVstRuntimeManager* juce_manager = nullptr
#endif
        ) :
            render_session(audio_device)
        {
#if IV_ENABLE_JUCE_VST
            if (juce_manager) {
                juce_vst_runtime = JuceVstRuntimeSupport(*juce_manager, static_cast<double>(audio_device.config().sample_rate));
            }
#endif
        }

        void register_runtime_buffers(TypeErasedAllocator allocator, InitBufferContext& context)
        {
#if IV_ENABLE_JUCE_VST
            juce_vst_runtime.register_runtime_buffers(allocator, context);
#endif
            size_t const num_channels = render_session.config().num_channels;
            if (context.mode == InitBufferContext::PassMode::initializing) {
                sink_bindings.clear();
            }

            for (size_t channel = 0; channel < num_channels; ++channel) {
                if (!context.has_tick_buffer(render_session.sink_id(channel))) {
                    continue;
                }

                auto buffer = allocator.template new_array<Sample>(render_session.sink_buffer_size());
                context.register_tick_buffer(render_session.sink_id(channel), buffer);
                if (context.mode == InitBufferContext::PassMode::initializing) {
                    sink_bindings.push_back({ channel, buffer });
                }
            }
        }

        size_t max_block_size() const
        {
            return render_session.scheduling_block_size();
        }

        size_t max_block_size(TypeErasedNode const&, size_t requested_max_block_size) const
        {
            return std::min(requested_max_block_size, max_block_size());
        }

        void clear_sink_buffers()
        {
            for (auto const& binding : sink_bindings) {
                auto buffer = binding.buffer;
                if (buffer.empty()) {
                    continue;
                }
                std::fill(buffer.begin(), buffer.end(), 0.0f);
            }
        }

        void mix_sink_buffers()
        {
            for (auto const& binding : sink_bindings) {
                render_session.mix_sink_buffer(binding.channel, binding.buffer);
            }
        }

        void tick_block(
            TypeErasedNode& root,
            std::span<std::byte> buffer,
            std::span<MidiMessage const> midi,
            size_t index,
            size_t block_size
        )
        {
            if (block_size == 0) {
                return;
            }

            size_t processed = 0;

            while (processed < block_size) {
                size_t block_index = index + processed;
                render_session.prepare_tick(block_index);
                if (render_session.is_shutdown_requested()) {
                    return;
                }

                if (!sink_bindings.empty() && block_index == render_session.active_block_start()) {
                    clear_sink_buffers();
                }

                size_t chunk_remaining = std::min(
                    block_size - processed,
                    render_session.active_block_end() - block_index
                );

                while (chunk_remaining != 0) {
                    size_t chunk_size = prev_power_of_2(chunk_remaining);
                    root.tick_block({
                        NodeState { .inputs = {}, .outputs = {}, .buffer = buffer },
                        midi,
                        block_index,
                        chunk_size,
                    });

                    processed += chunk_size;
                    block_index += chunk_size;
                    chunk_remaining -= chunk_size;
                }

                if (!sink_bindings.empty() && block_index == render_session.active_block_end()) {
                    mix_sink_buffers();
                }

                render_session.finish_tick(block_index - 1);
            }
        }
    };

    struct FileRenderRuntime {
        RenderConfig config;
        std::string output_path;
        std::vector<std::string> sink_ids;
#if IV_ENABLE_JUCE_VST
        JuceVstRuntimeSupport juce_vst_runtime;
#endif

        struct SinkBinding {
            size_t channel = 0;
            std::span<Sample> ring;
            std::vector<Sample> captured;
        };

        std::vector<SinkBinding> sink_bindings;

        explicit FileRenderRuntime(RenderConfig config_, std::string output_path_, std::vector<std::string> sink_ids_) :
            config(config_),
            output_path(std::move(output_path_)),
            sink_ids(std::move(sink_ids_))
        {}

        ~FileRenderRuntime()
        {
            flush();
        }

        void register_runtime_buffers(TypeErasedAllocator allocator, InitBufferContext& context)
        {
#if IV_ENABLE_JUCE_VST
            if (juce_vst_runtime) {
                juce_vst_runtime.register_runtime_buffers(allocator, context);
            }
#endif
            if (context.mode == InitBufferContext::PassMode::initializing) {
                sink_bindings.clear();
            }

            size_t const ring_size = size_t(1) << size_t(std::ceil(std::log2(std::max<size_t>(context.max_block_size, 1))));
            for (size_t channel = 0; channel < config.num_channels; ++channel) {
                if (channel >= sink_ids.size()) {
                    continue;
                }
                std::string const& id = sink_ids[channel];
                if (!context.has_tick_buffer(id)) {
                    continue;
                }

                auto buffer = allocator.template new_array<Sample>(ring_size);
                context.register_tick_buffer(id, buffer);
                if (context.mode == InitBufferContext::PassMode::initializing) {
                    sink_bindings.push_back({ channel, buffer, {} });
                }
            }
        }

        size_t max_block_size(TypeErasedNode const&, size_t requested_max_block_size) const
        {
            return std::min(requested_max_block_size, prev_power_of_2(config.max_block_frames));
        }

        void clear_window(size_t index, size_t frames)
        {
            for (auto& binding : sink_bindings) {
                for (size_t frame = 0; frame < frames; ++frame) {
                    binding.ring[(index + frame) & (binding.ring.size() - 1)] = 0.0f;
                }
            }
        }

        void capture_window(size_t index, size_t frames)
        {
            for (auto& binding : sink_bindings) {
                size_t const end = index + frames;
                if (binding.captured.size() < end) {
                    binding.captured.resize(end, 0.0f);
                }
                for (size_t frame = 0; frame < frames; ++frame) {
                    binding.captured[index + frame] += binding.ring[(index + frame) & (binding.ring.size() - 1)];
                }
            }
        }

        void tick_block(
            TypeErasedNode& root,
            std::span<std::byte> buffer,
            std::span<MidiMessage const> midi,
            size_t index,
            size_t block_size
        )
        {
            clear_window(index, block_size);
            root.tick_block({
                NodeState { .inputs = {}, .outputs = {}, .buffer = buffer },
                midi,
                index,
                block_size
            });
            capture_window(index, block_size);
        }

        void flush()
        {
            if (output_path.empty() || sink_bindings.empty()) {
                return;
            }

            size_t frames = 0;
            for (auto const& binding : sink_bindings) {
                frames = std::max(frames, binding.captured.size());
            }
            if (frames == 0) {
                return;
            }

            std::vector<Sample> left(frames, 0.0f);
            std::vector<Sample> right(frames, 0.0f);
            for (auto const& binding : sink_bindings) {
                if (binding.channel == 0) {
                    std::copy(binding.captured.begin(), binding.captured.end(), left.begin());
                } else if (binding.channel == 1) {
                    std::copy(binding.captured.begin(), binding.captured.end(), right.begin());
                }
            }
            if (sink_bindings.size() == 1) {
                right = left;
            }
            write_wav(output_path, left, right, static_cast<uint32_t>(config.sample_rate));
            output_path.clear();
        }
    };

    struct PassiveRuntime {
#if IV_ENABLE_JUCE_VST
        JuceVstRuntimeSupport juce_vst_runtime;

        explicit PassiveRuntime(JuceVstRuntimeManager& manager, double sample_rate) :
            juce_vst_runtime(manager, sample_rate)
        {}
#else
        PassiveRuntime() = default;
#endif

        void register_runtime_buffers(TypeErasedAllocator allocator, InitBufferContext& context)
        {
#if IV_ENABLE_JUCE_VST
            juce_vst_runtime.register_runtime_buffers(allocator, context);
#else
            (void)allocator;
            (void)context;
#endif
        }

        size_t max_block_size(TypeErasedNode const&, size_t requested_max_block_size) const
        {
            return requested_max_block_size;
        }

        void tick_block(
            TypeErasedNode& root,
            std::span<std::byte> buffer,
            std::span<MidiMessage const> midi,
            size_t index,
            size_t block_size
        )
        {
            root.tick_block({
                NodeState { .inputs = {}, .outputs = {}, .buffer = buffer },
                midi,
                index,
                block_size
            });
        }
    };

    class System {
        AudioDevice _audio_device;
        bool _close_on_enter = true;
        std::jthread _close_thread;
        bool _active_root_uses_audio_device = false;
#if IV_ENABLE_JUCE_VST
        JuceVstRuntimeManager _juce_vst_runtime_manager;
#endif

    public:
        explicit System(RenderConfig config = {}, bool open_backend = true, bool close_on_enter = true) :
            _audio_device(config, open_backend),
            _close_on_enter(close_on_enter)
        {
            if (_close_on_enter) {
                _close_thread = std::jthread([this](std::stop_token) {
                    std::string line;
                    std::getline(std::cin, line);
                    _audio_device.request_shutdown();
                });
            }
        }

        ~System()
        {
            request_shutdown();
        }

        System(System const&) = delete;
        System& operator=(System const&) = delete;

        AudioDevice& audio_device()
        {
            return _audio_device;
        }

        AudioDevice const& audio_device() const
        {
            return _audio_device;
        }

        RenderConfig const& render_config() const
        {
            return _audio_device.config();
        }

        Sample& sample_period()
        {
            return _audio_device.sample_period();
        }

        size_t preferred_block_size() const
        {
            return render_config().preferred_block_size;
        }

        size_t root_block_size() const
        {
            return (_active_root_uses_audio_device && !file_render_path_env())
                ? _audio_device.scheduling_block_size()
                : prev_power_of_2(render_config().max_block_frames);
        }

        bool is_shutdown_requested() const
        {
            return _audio_device.is_shutdown_requested();
        }

        void request_shutdown()
        {
            _audio_device.request_shutdown();
        }

        void activate_root(bool uses_audio_device)
        {
            _active_root_uses_audio_device = uses_audio_device;
            if (_active_root_uses_audio_device && !file_render_path_env()) {
                _audio_device.ensure_backend_initialized();
            }
        }

        NodeProcessor make_processor(TypeErasedNode root, std::vector<std::shared_ptr<void>> module_refs = {})
        {
            TypeErasedNodeRuntime runtime;
            if (_active_root_uses_audio_device) {
                if (char const* file_path = file_render_path_env()) {
                    std::vector<std::string> sink_ids(render_config().num_channels);
                    for (size_t channel = 0; channel < sink_ids.size(); ++channel) {
                        sink_ids[channel] = _audio_device.sink_id(channel);
                    }
                    auto file_runtime = FileRenderRuntime(render_config(), std::string(file_path), std::move(sink_ids));
#if IV_ENABLE_JUCE_VST
                    file_runtime.juce_vst_runtime = JuceVstRuntimeSupport(_juce_vst_runtime_manager, static_cast<double>(render_config().sample_rate));
#endif
                    runtime = TypeErasedNodeRuntime(std::move(file_runtime));
                } else {
                    _audio_device.ensure_backend_initialized();
                    runtime = TypeErasedNodeRuntime(SystemProcessorRuntime(
                        _audio_device
#if IV_ENABLE_JUCE_VST
                        , &_juce_vst_runtime_manager
#endif
                    ));
                }
#if IV_ENABLE_JUCE_VST
            } else {
                runtime = TypeErasedNodeRuntime(PassiveRuntime(_juce_vst_runtime_manager, static_cast<double>(render_config().sample_rate)));
#endif
            }
            size_t const root_max_block_size = root.max_block_size();
            size_t const runtime_max_block_size = runtime
                ? runtime.max_block_size(root, root_max_block_size)
                : root_max_block_size;
            size_t const processor_block_size = std::min(
                root_block_size(),
                runtime_max_block_size
            );
            return NodeProcessor(
                std::move(root),
                std::move(module_refs),
                processor_block_size,
                std::move(runtime)
            );
        }
    };
}
