#include "juce_vst_runtime.h"

#if IV_ENABLE_JUCE_VST

#include "../juce_vst_wrapper.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace iv {
    namespace {
        uint64_t fnv1a_append(uint64_t hash, std::string_view text)
        {
            for (unsigned char c : text) {
                hash ^= c;
                hash *= 1099511628211ull;
            }
            return hash;
        }

        uint64_t make_schema_fingerprint(size_t audio_inputs, size_t audio_outputs, std::span<JuceVstParameterSpec const> parameters)
        {
            uint64_t hash = 1469598103934665603ull;
            hash = fnv1a_append(hash, std::to_string(audio_inputs));
            hash = fnv1a_append(hash, std::to_string(audio_outputs));
            for (auto const& parameter : parameters) {
                hash = fnv1a_append(hash, parameter.id);
                hash = fnv1a_append(hash, parameter.name);
                hash = fnv1a_append(hash, std::to_string(parameter.default_value));
            }
            return hash;
        }

        std::string canonical_plugin_path(std::filesystem::path const& path)
        {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(path, ec);
            if (!ec) {
                return canonical.generic_string();
            }
            return std::filesystem::absolute(path).lexically_normal().generic_string();
        }

        ::juce::String to_juce_string(std::filesystem::path const& path)
        {
            return ::juce::String(path.string());
        }

        std::runtime_error make_probe_error(std::string const& message, JuceVstPluginConfig const& config)
        {
            return std::runtime_error(message + ": " + canonical_plugin_path(config.plugin_path));
        }

        std::string make_audio_port_name(size_t channel)
        {
            return std::string(channel % 2 == 0 ? "l" : "r") + std::to_string(channel / 2);
        }

        size_t saturating_stream_channels(size_t streams)
        {
            if (streams > std::numeric_limits<size_t>::max() / 2) {
                return std::numeric_limits<size_t>::max();
            }
            return streams * 2;
        }

        ::juce::PluginDescription resolve_plugin_description(
            ::juce::AudioPluginFormatManager& format_manager,
            JuceVstPluginConfig const& config
        )
        {
            ::juce::OwnedArray<::juce::PluginDescription> descriptions;
            ::juce::String const file_path = to_juce_string(config.plugin_path);
            for (auto* format : format_manager.getFormats()) {
                if (!format->fileMightContainThisPluginType(file_path)) {
                    continue;
                }
                format->findAllTypesForFile(descriptions, file_path);
            }

            if (descriptions.isEmpty()) {
                throw make_probe_error("no JUCE-hostable plugins found in file", config);
            }

            if (!config.plugin_identifier.empty()) {
                for (auto const* description : descriptions) {
                    if (description->createIdentifierString().toStdString() == config.plugin_identifier) {
                        return *description;
                    }
                }
                throw make_probe_error("plugin identifier was not found in file", config);
            }

            if (descriptions.size() != 1) {
                throw make_probe_error("plugin file contains multiple JUCE plugin descriptions but no identifier was provided", config);
            }

            return *descriptions.getFirst();
        }

        std::unique_ptr<::juce::AudioPluginInstance> create_plugin_instance(
            ::juce::AudioPluginFormatManager& format_manager,
            JuceVstPluginConfig const& config,
            double sample_rate,
            int block_size
        )
        {
            ::juce::PluginDescription const description = resolve_plugin_description(format_manager, config);
            ::juce::String error;
            auto instance = format_manager.createPluginInstance(description, sample_rate, block_size, error);
            if (!instance) {
                throw std::runtime_error("JUCE failed to create plugin instance '" + description.createIdentifierString().toStdString() + "': " + error.toStdString());
            }
            return instance;
        }

        bool try_set_bus_layout(
            ::juce::AudioPluginInstance& instance,
            bool is_input,
            std::span<::juce::AudioChannelSet const> bus_layouts
        )
        {
            int const bus_count = instance.getBusCount(is_input);
            if (bus_layouts.size() != static_cast<size_t>(bus_count)) {
                return false;
            }

            for (int bus_index = 0; bus_index < bus_count; ++bus_index) {
                if (!instance.setChannelLayoutOfBus(is_input, bus_index, bus_layouts[bus_index])) {
                    return false;
                }
            }
            return true;
        }

        size_t count_layout_channels(std::span<::juce::AudioChannelSet const> bus_layouts)
        {
            size_t total = 0;
            for (auto const& layout : bus_layouts) {
                total += static_cast<size_t>(layout.size());
            }
            return total;
        }

        size_t configure_audio_direction(
            ::juce::AudioPluginInstance& instance,
            bool is_input,
            size_t preferred_streams
        )
        {
            int const bus_count = instance.getBusCount(is_input);
            if (bus_count <= 0) {
                return 0;
            }

            size_t const desired_channels = saturating_stream_channels(preferred_streams);
            std::vector<size_t> candidate_active_buses;
            candidate_active_buses.reserve(static_cast<size_t>(bus_count) + 1);
            auto push_candidate = [&](size_t active_buses) {
                if (active_buses > static_cast<size_t>(bus_count)) {
                    return;
                }
                if (std::find(candidate_active_buses.begin(), candidate_active_buses.end(), active_buses) == candidate_active_buses.end()) {
                    candidate_active_buses.push_back(active_buses);
                }
            };

            size_t const desired_buses = std::min(preferred_streams, static_cast<size_t>(bus_count));
            push_candidate(desired_buses);
            for (size_t delta = 1; delta <= static_cast<size_t>(bus_count); ++delta) {
                if (desired_buses >= delta) {
                    push_candidate(desired_buses - delta);
                }
                push_candidate(desired_buses + delta);
            }

            for (size_t active_buses : candidate_active_buses) {
                std::vector<::juce::AudioChannelSet> layouts(static_cast<size_t>(bus_count), ::juce::AudioChannelSet::disabled());
                for (size_t bus_index = 0; bus_index < active_buses; ++bus_index) {
                    layouts[bus_index] = ::juce::AudioChannelSet::stereo();
                }
                if (try_set_bus_layout(instance, is_input, layouts)) {
                    return count_layout_channels(layouts);
                }
            }

            std::vector<size_t> candidate_channels;
            candidate_channels.reserve(std::max<size_t>(8, desired_channels / 2 + 2));
            auto push_channel_candidate = [&](size_t channels) {
                if (std::find(candidate_channels.begin(), candidate_channels.end(), channels) == candidate_channels.end()) {
                    candidate_channels.push_back(channels);
                }
            };

            push_channel_candidate(desired_channels);
            for (size_t delta = 2; delta <= std::max<size_t>(desired_channels, 16); delta += 2) {
                if (desired_channels >= delta) {
                    push_channel_candidate(desired_channels - delta);
                }
                push_channel_candidate(desired_channels + delta);
            }
            push_channel_candidate(1);
            push_channel_candidate(0);

            for (size_t channels : candidate_channels) {
                std::vector<::juce::AudioChannelSet> layouts(static_cast<size_t>(bus_count), ::juce::AudioChannelSet::disabled());
                layouts[0] = channels == 0
                    ? ::juce::AudioChannelSet::disabled()
                    : ::juce::AudioChannelSet::discreteChannels(static_cast<int>(channels));
                if (try_set_bus_layout(instance, is_input, layouts)) {
                    return count_layout_channels(layouts);
                }
            }

            return static_cast<size_t>(is_input ? instance.getTotalNumInputChannels() : instance.getTotalNumOutputChannels());
        }

        void configure_audio_layout(::juce::AudioPluginInstance& instance, JuceVstPluginConfig const& config)
        {
            configure_audio_direction(instance, true, config.preferred_audio_input_streams);
            configure_audio_direction(instance, false, config.preferred_audio_output_streams);
        }

        JuceVstParameterSpec make_parameter_spec(::juce::AudioProcessorParameter& parameter, size_t index)
        {
            JuceVstParameterSpec spec;
            if (auto* with_id = dynamic_cast<::juce::AudioProcessorParameterWithID*>(&parameter)) {
                spec.id = with_id->paramID.toStdString();
            } else {
                spec.id = "param_" + std::to_string(index);
            }
            spec.name = parameter.getName(128).toStdString();
            if (spec.name.empty()) {
                spec.name = spec.id;
            }
            spec.default_value = parameter.getDefaultValue();
            return spec;
        }

        struct ParameterBinding {
            ::juce::AudioProcessorParameter* parameter = nullptr;
            float last_value = std::numeric_limits<float>::quiet_NaN();
        };
    }

    struct JuceVstRuntimeManager::LiveInstance {
        std::shared_ptr<JuceVstWrapperSpec const> spec;
        std::unique_ptr<::juce::AudioPluginInstance> plugin;
        ::juce::AudioBuffer<float> audio_buffer;
        ::juce::MidiBuffer midi_buffer;
        std::vector<ParameterBinding> parameter_bindings;
        double prepared_sample_rate = 0.0;
        int prepared_block_size = 0;
        std::mutex mutex;

        void prepare(double sample_rate, size_t block_size)
        {
            std::lock_guard lock(mutex);
            if (prepared_sample_rate == sample_rate && prepared_block_size == static_cast<int>(block_size)) {
                return;
            }

            plugin->suspendProcessing(true);
            plugin->releaseResources();
            plugin->setRateAndBufferSizeDetails(sample_rate, static_cast<int>(block_size));
            plugin->prepareToPlay(sample_rate, static_cast<int>(block_size));
            plugin->suspendProcessing(false);

            prepared_sample_rate = sample_rate;
            prepared_block_size = static_cast<int>(block_size);
            audio_buffer.setSize(
                static_cast<int>(std::max(spec->schema.audio_inputs, spec->schema.audio_outputs)),
                static_cast<int>(block_size),
                false,
                true,
                true
            );
        }
    };

    struct JuceVstRuntimeManager::Impl {
        ::juce::AudioPluginFormatManager format_manager;

        Impl()
        {
            format_manager.addDefaultFormats();
        }
    };

    JuceVstRuntimeManager::JuceVstRuntimeManager() :
        _impl(std::make_unique<Impl>())
    {}

    JuceVstRuntimeManager::~JuceVstRuntimeManager() = default;

    JuceVstRuntimeSupport::JuceVstRuntimeSupport(JuceVstRuntimeManager& manager, double sample_rate) :
        _manager(&manager),
        _sample_rate(sample_rate),
        _vst_resources{
            .owner = this,
            .create_juce_vst_fn = [](void* owner, JuceVstWrapperSpec const& spec) {
                auto& support = *static_cast<JuceVstRuntimeSupport*>(owner);
                return support._manager->create_instance(spec, support._sample_rate);
            },
        },
        _resources { .vst = _vst_resources }
    {}

    JuceVstWrapperSpec probe_juce_vst(JuceVstPluginConfig request)
    {
        ::juce::AudioPluginFormatManager format_manager;
        format_manager.addDefaultFormats();

        ::juce::PluginDescription const description = resolve_plugin_description(format_manager, request);
        auto instance = create_plugin_instance(format_manager, request, 48000.0, 512);
        configure_audio_layout(*instance, request);

        JuceVstWrapperSpec spec;
        spec.plugin.plugin_path = std::filesystem::weakly_canonical(request.plugin_path);
        spec.plugin.plugin_identifier = description.createIdentifierString().toStdString();
        spec.plugin.preferred_audio_input_streams = request.preferred_audio_input_streams;
        spec.plugin.preferred_audio_output_streams = request.preferred_audio_output_streams;
        spec.schema.audio_inputs = static_cast<size_t>(instance->getTotalNumInputChannels());
        spec.schema.audio_outputs = static_cast<size_t>(instance->getTotalNumOutputChannels());
        spec.schema.audio_input_names.reserve(spec.schema.audio_inputs);
        for (size_t channel = 0; channel < spec.schema.audio_inputs; ++channel) {
            spec.schema.audio_input_names.push_back(make_audio_port_name(channel));
        }
        spec.schema.audio_output_names.reserve(spec.schema.audio_outputs);
        for (size_t channel = 0; channel < spec.schema.audio_outputs; ++channel) {
            spec.schema.audio_output_names.push_back(make_audio_port_name(channel));
        }
        auto& parameters = instance->getParameters();
        spec.schema.parameters.reserve(parameters.size());
        for (int index = 0; index < parameters.size(); ++index) {
            spec.schema.parameters.push_back(make_parameter_spec(*parameters[index], size_t(index)));
        }
        spec.schema.fingerprint = make_schema_fingerprint(
            spec.schema.audio_inputs,
            spec.schema.audio_outputs,
            spec.schema.parameters
        );
        return spec;
    }

    UniqueResource JuceVstRuntimeManager::create_instance(
        JuceVstWrapperSpec const& spec,
        double sample_rate
    )
    {
        std::lock_guard lock(_mutex);
        auto instance = create_plugin_instance(
            _impl->format_manager,
            spec.plugin,
            sample_rate > 0.0 ? sample_rate : 48000.0,
            512
        );
        configure_audio_layout(*instance, spec.plugin);

        auto live = std::make_unique<LiveInstance>();
        live->spec = std::make_shared<JuceVstWrapperSpec const>(spec);
        live->plugin = std::move(instance);
        live->parameter_bindings.reserve(live->plugin->getParameters().size());
        for (auto* parameter : live->plugin->getParameters()) {
            live->parameter_bindings.push_back(ParameterBinding{ parameter, std::numeric_limits<float>::quiet_NaN() });
        }
        return UniqueResource(
            live.release(),
            +[](void* ptr) {
                delete static_cast<LiveInstance*>(ptr);
            }
        );
    }

    void JuceVstRuntimeSupport::register_runtime_buffers(TypeErasedAllocator allocator, NodeLayoutBuilder& builder)
    {
        (void)allocator;
        (void)builder;
    }

    void tick_juce_vst_wrapper(
        JuceVstWrapperSpec const& spec,
        void* live_instance_ptr,
        TickBlockContext<JuceVstWrapper> const& state
    )
    {
        if (live_instance_ptr == nullptr) {
            throw std::logic_error("JuceVstWrapper was ticked without a bound runtime instance");
        }

        auto& live_instance = *static_cast<JuceVstRuntimeManager::LiveInstance*>(live_instance_ptr);
        std::lock_guard lock(live_instance.mutex);

        auto& buffer = live_instance.audio_buffer;

        for (size_t channel = 0; channel < spec.schema.audio_inputs; ++channel) {
            auto const block = state.inputs[channel].get_block(state.block_size);
            float* destination = buffer.getWritePointer(static_cast<int>(channel));
            for (size_t sample = 0; sample < state.block_size; ++sample) {
                destination[sample] = block[sample];
            }
        }

        size_t const parameter_offset = spec.schema.audio_inputs;
        for (size_t index = 0; index < live_instance.parameter_bindings.size(); ++index) {
            float value = state.inputs[parameter_offset + index].get();
            value = std::clamp(value, 0.0f, 1.0f);
            auto& binding = live_instance.parameter_bindings[index];
            if (binding.last_value != value) {
                binding.parameter->setValue(value);
                binding.last_value = value;
            }
        }

        live_instance.midi_buffer.clear();
        live_instance.plugin->processBlock(buffer, live_instance.midi_buffer);

        for (size_t channel = 0; channel < spec.schema.audio_outputs; ++channel) {
            Sample::storage const* source_storage = buffer.getReadPointer(static_cast<int>(channel));
            Sample const* source = reinterpret_cast<Sample const*>(source_storage);
            state.outputs[channel].push_block(std::span<Sample const>(source, state.block_size));
        }
    }

    void JuceVstWrapper::tick_block(TickBlockContext<JuceVstWrapper> const& ctx) const
    {
        tick_juce_vst_wrapper(*_spec, ctx.state().plugin_instance.get(), ctx);
    }
}

#endif
