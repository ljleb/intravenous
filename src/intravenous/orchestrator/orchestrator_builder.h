#pragma once

#include "device_orchestrator.h"
#include "devices/wav_file_device.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace iv {
    class NodeExecutor;

    class OrchestratorBuilder {
        friend class NodeExecutor;

        class ChannelSinks {
            std::vector<std::span<Sample>> _sinks;

            static bool same_span(std::span<Sample> a, std::span<Sample> b)
            {
                return a.data() == b.data() && a.size() == b.size();
            }

        public:
            void register_sink(std::span<Sample> buffer)
            {
                auto it = std::find_if(_sinks.begin(), _sinks.end(), [&](std::span<Sample> existing) {
                    return same_span(existing, buffer);
                });
                if (it == _sinks.end()) {
                    _sinks.push_back(buffer);
                }
            }

            void update_sink(std::span<Sample> previous_buffer, std::span<Sample> buffer)
            {
                auto it = std::find_if(_sinks.begin(), _sinks.end(), [&](std::span<Sample> existing) {
                    return same_span(existing, previous_buffer);
                });
                if (it != _sinks.end()) {
                    *it = buffer;
                    return;
                }
                register_sink(buffer);
            }

            void unregister_sink(std::span<Sample> buffer)
            {
                auto it = std::find_if(_sinks.begin(), _sinks.end(), [&](std::span<Sample> existing) {
                    return same_span(existing, buffer);
                });
                if (it != _sinks.end()) {
                    _sinks.erase(it);
                }
            }

            bool empty() const
            {
                return _sinks.empty();
            }

            std::span<std::span<Sample> const> sinks() const
            {
                return _sinks;
            }
        };

    public:
        struct AudioDeviceBinding {
            friend class OrchestratorBuilder;

        private:
            size_t device_id;
            std::vector<ChannelSinks> channels;
            std::optional<OutputDeviceMixer> mixer;

        public:
            AudioDeviceBinding() = default;

            explicit AudioDeviceBinding(size_t device_id)
            : device_id(device_id)
            {}

            void register_sink(size_t channel, std::span<Sample> buffer)
            {
                ensure_channel(channel).register_sink(buffer);
                if (mixer.has_value()) {
                    mixer->register_sink(channel, buffer);
                }
            }

            void update_sink(size_t channel, std::span<Sample> previous_buffer, std::span<Sample> buffer)
            {
                ensure_channel(channel).update_sink(previous_buffer, buffer);
                if (mixer.has_value()) {
                    mixer->update_sink(channel, previous_buffer, buffer);
                }
            }

            void unregister_sink(size_t channel, std::span<Sample> buffer)
            {
                ensure_channel(channel).unregister_sink(buffer);
                if (mixer.has_value()) {
                    mixer->unregister_sink(channel, buffer);
                }
            }

            bool is_used() const
            {
                return std::any_of(channels.begin(), channels.end(), [](ChannelSinks const& channel) {
                    return !channel.empty();
                });
            }

            void bind_mixer(OutputDeviceMixer next_mixer)
            {
                mixer.emplace(std::move(next_mixer));
                replay_sinks();
            }

        private:
            ChannelSinks& ensure_channel(size_t channel)
            {
                if (channels.size() <= channel) {
                    channels.resize(channel + 1);
                }
                return channels[channel];
            }

            void replay_sinks()
            {
                if (!mixer.has_value()) {
                    return;
                }

                for (size_t channel = 0; channel < channels.size(); ++channel) {
                    for (auto sink : channels[channel].sinks()) {
                        mixer->register_sink(channel, sink);
                    }
                }
            }
        };

        struct FileBinding {
            friend class OrchestratorBuilder;

        private:
            std::filesystem::path path;
            std::vector<ChannelSinks> channels;
            std::optional<OutputDeviceMixer> mixer;

        public:
            FileBinding() = default;

            explicit FileBinding(std::filesystem::path path)
            : path(std::move(path))
            {}

            void register_sink(size_t channel, std::span<Sample> buffer)
            {
                ensure_channel(channel).register_sink(buffer);
                if (mixer.has_value()) {
                    mixer->register_sink(channel, buffer);
                }
            }

            void update_sink(size_t channel, std::span<Sample> previous_buffer, std::span<Sample> buffer)
            {
                ensure_channel(channel).update_sink(previous_buffer, buffer);
                if (mixer.has_value()) {
                    mixer->update_sink(channel, previous_buffer, buffer);
                }
            }

            void unregister_sink(size_t channel, std::span<Sample> buffer)
            {
                ensure_channel(channel).unregister_sink(buffer);
                if (mixer.has_value()) {
                    mixer->unregister_sink(channel, buffer);
                }
            }

            bool is_used() const
            {
                return std::any_of(channels.begin(), channels.end(), [](ChannelSinks const& channel) {
                    return !channel.empty();
                });
            }

            size_t num_channels() const
            {
                size_t count = 0;
                for (size_t channel = 0; channel < channels.size(); ++channel) {
                    if (!channels[channel].empty()) {
                        count = channel + 1;
                    }
                }
                return std::max<size_t>(count, 1);
            }

            void bind_mixer(OutputDeviceMixer next_mixer)
            {
                mixer.emplace(std::move(next_mixer));
                replay_sinks();
            }

        private:
            ChannelSinks& ensure_channel(size_t channel)
            {
                if (channels.size() <= channel) {
                    channels.resize(channel + 1);
                }
                return channels[channel];
            }

            void replay_sinks()
            {
                if (!mixer.has_value()) {
                    return;
                }

                for (size_t channel = 0; channel < channels.size(); ++channel) {
                    for (auto sink : channels[channel].sinks()) {
                        mixer->register_sink(channel, sink);
                    }
                }
            }
        };

    private:
        std::vector<AudioDeviceBinding> _audio_devices;
        std::vector<FileBinding> _files;

        AudioDeviceBinding& ensure_audio_device(size_t device_id)
        {
            auto it = std::find_if(_audio_devices.begin(), _audio_devices.end(), [&](AudioDeviceBinding const& binding) {
                return binding.device_id == device_id;
            });
            if (it == _audio_devices.end()) {
                _audio_devices.emplace_back(device_id);
                return _audio_devices.back();
            }
            return *it;
        }

        FileBinding& ensure_file(std::filesystem::path const& path)
        {
            auto it = std::find_if(_files.begin(), _files.end(), [&](FileBinding const& binding) {
                return binding.path == path;
            });
            if (it == _files.end()) {
                _files.emplace_back(path);
                return _files.back();
            }
            return *it;
        }

        static OutputDeviceMixer make_audio_mixer(size_t device_id, LogicalAudioDevice device)
        {
            return OutputDeviceMixer(
                std::move(device),
                [device_id](OrchestratorBuilder& builder, OutputDeviceMixer&& mixer) {
                    builder.insert_audio_mixer(device_id, std::move(mixer));
                }
            );
        }

        static OutputDeviceMixer make_file_mixer(std::filesystem::path path, LogicalAudioDevice device)
        {
            return OutputDeviceMixer(
                std::move(device),
                [path = std::move(path)](OrchestratorBuilder& builder, OutputDeviceMixer&& mixer) mutable {
                    builder.insert_file_mixer(std::move(path), std::move(mixer));
                }
            );
        }

        void insert_audio_mixer(size_t device_id, OutputDeviceMixer mixer)
        {
            ensure_audio_device(device_id).bind_mixer(std::move(mixer));
        }

        void insert_file_mixer(std::filesystem::path path, OutputDeviceMixer mixer)
        {
            ensure_file(path).bind_mixer(std::move(mixer));
        }

        std::optional<RenderConfig> reference_render_config() const
        {
            for (auto const& binding : _audio_devices) {
                if (binding.mixer.has_value()) {
                    return binding.mixer->config();
                }
            }
            for (auto const& binding : _files) {
                if (binding.mixer.has_value()) {
                    return binding.mixer->config();
                }
            }
            return std::nullopt;
        }

        RenderConfig file_render_config(FileBinding const& binding) const
        {
            RenderConfig config = reference_render_config().value_or(RenderConfig {});
            config.num_channels = binding.num_channels();
            return config;
        }

        void ensure_file_mixers()
        {
            for (auto& binding : _files) {
                if (!binding.is_used() || binding.mixer.has_value()) {
                    continue;
                }
                binding.bind_mixer(make_file_mixer(
                    binding.path,
                    make_wav_file_device(binding.path, file_render_config(binding))
                ));
            }
        }

        size_t preferred_block_size_hint() const
        {
            auto block_hint = [](RenderConfig const& config) {
                return std::min(config.preferred_block_size, config.max_block_frames);
            };

            auto update_hint = [&](size_t& hint, bool& found, auto const& bindings, bool used_only) {
                for (auto const& binding : bindings) {
                    if (!binding.mixer.has_value()) {
                        continue;
                    }
                    if (used_only && !binding.is_used()) {
                        continue;
                    }
                    hint = std::min(hint, block_hint(binding.mixer->config()));
                    found = true;
                }
            };

            size_t hint = MAX_BLOCK_SIZE;
            bool found = false;
            update_hint(hint, found, _audio_devices, true);
            update_hint(hint, found, _files, true);
            if (found) {
                return hint;
            }

            update_hint(hint, found, _audio_devices, false);
            update_hint(hint, found, _files, false);
            return found ? hint : MAX_BLOCK_SIZE;
        }

    public:
        OrchestratorBuilder() = default;
        OrchestratorBuilder(OrchestratorBuilder&&) noexcept = default;
        OrchestratorBuilder& operator=(OrchestratorBuilder&&) noexcept = default;
        OrchestratorBuilder(OrchestratorBuilder const&) = delete;
        OrchestratorBuilder& operator=(OrchestratorBuilder const&) = delete;

        AudioDeviceBinding& audio_device(size_t device_id, [[maybe_unused]] size_t channel)
        {
            return ensure_audio_device(device_id);
        }

        FileBinding& file(std::filesystem::path const& path, [[maybe_unused]] size_t channel)
        {
            return ensure_file(path);
        }

        void add_audio_device(size_t device_id, LogicalAudioDevice device)
        {
            insert_audio_mixer(device_id, make_audio_mixer(device_id, std::move(device)));
        }

        void add_audio_mixer(size_t device_id, OutputDeviceMixer mixer)
        {
            insert_audio_mixer(device_id, std::move(mixer));
        }

        void add_file_device(std::filesystem::path path, LogicalAudioDevice device)
        {
            insert_file_mixer(path, make_file_mixer(std::move(path), std::move(device)));
        }

        void add_file_mixer(std::filesystem::path path, OutputDeviceMixer mixer)
        {
            insert_file_mixer(std::move(path), std::move(mixer));
        }

        DeviceOrchestrator build() &&
        {
            ensure_file_mixers();

            std::vector<OutputDeviceMixer> mixers;
            for (auto& binding : _audio_devices) {
                if (binding.is_used() && binding.mixer.has_value()) {
                    mixers.push_back(std::move(*binding.mixer));
                    binding.mixer.reset();
                }
            }
            for (auto& binding : _files) {
                if (binding.is_used() && binding.mixer.has_value()) {
                    mixers.push_back(std::move(*binding.mixer));
                    binding.mixer.reset();
                }
            }
            return DeviceOrchestrator(std::move(mixers));
        }
    };

    inline OrchestratorBuilder DeviceOrchestrator::to_builder() &&
    {
        OrchestratorBuilder builder;
        for (auto& mixer : _mixers) {
            std::move(mixer).move_to_builder(builder);
        }
        return builder;
    }
}
