#pragma once

#include "dsl.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef IV_ENABLE_JUCE_VST
#define IV_ENABLE_JUCE_VST 0
#endif

namespace iv {
    namespace juce {
#if !IV_ENABLE_JUCE_VST
        template<typename... Args>
        NodeRef vst(Args&&...)
        {
            static_assert(false, "iv::juce::vst(...) requires JUCE VST support. Configure the project with JUCE available so IV_ENABLE_JUCE_VST=1.");
            return NodeRef();
        }
#endif
    }

#if IV_ENABLE_JUCE_VST
    class JuceVstWrapper;

    struct JuceVstParameterSpec {
        std::string id;
        std::string name;
        Sample default_value = 0.0f;
    };

    struct JuceVstSchema {
        size_t audio_inputs = 0;
        size_t audio_outputs = 0;
        std::vector<std::string> audio_input_names;
        std::vector<std::string> audio_output_names;
        std::vector<JuceVstParameterSpec> parameters;
        uint64_t fingerprint = 0;
    };

    struct JuceVstPluginConfig {
        std::filesystem::path plugin_path;
        std::string plugin_identifier;
        size_t preferred_audio_input_streams = 1;
        size_t preferred_audio_output_streams = 1;
    };

    struct JuceVstProbeRequest {
        std::string resource_id;
        JuceVstPluginConfig plugin;
    };

    struct JuceVstWrapperSpec {
        std::string resource_id;
        JuceVstPluginConfig plugin;
        JuceVstSchema schema;
    };

    JuceVstWrapperSpec probe_juce_vst(JuceVstProbeRequest request);

    namespace juce {
        inline NodeRef vst(
            GraphBuilder& g,
            std::string resource_id,
            std::filesystem::path plugin_path,
            std::string plugin_identifier = {},
            size_t preferred_audio_input_streams = 1,
            size_t preferred_audio_output_streams = 1
        )
        {
            return g.node<JuceVstWrapper>(probe_juce_vst(JuceVstProbeRequest{
                .resource_id = std::move(resource_id),
                .plugin = JuceVstPluginConfig{
                    .plugin_path = std::move(plugin_path),
                    .plugin_identifier = std::move(plugin_identifier),
                    .preferred_audio_input_streams = preferred_audio_input_streams,
                    .preferred_audio_output_streams = preferred_audio_output_streams,
                },
            }));
        }
    }

    namespace juce_details {
        std::string make_juce_vst_runtime_resource_id(std::string_view resource_id);
        std::string make_juce_vst_spec_buffer_id(std::string_view resource_id);
    }

    class JuceVstWrapper {
        std::shared_ptr<JuceVstWrapperSpec const> _spec;

    public:
        struct State {
            void** runtime_slot = nullptr;
        };

        explicit JuceVstWrapper(JuceVstWrapperSpec spec) :
            _spec(std::make_shared<JuceVstWrapperSpec const>(std::move(spec)))
        {}

        std::vector<InputConfig> inputs() const
        {
            std::vector<InputConfig> inputs;
            inputs.reserve(_spec->schema.audio_inputs + _spec->schema.parameters.size());
            for (size_t channel = 0; channel < _spec->schema.audio_inputs; ++channel) {
                inputs.push_back(InputConfig{
                    .name = _spec->schema.audio_input_names[channel],
                });
            }
            for (auto const& parameter : _spec->schema.parameters) {
                inputs.push_back(InputConfig{
                    .name = parameter.name,
                    .default_value = parameter.default_value,
                });
            }
            return inputs;
        }

        std::vector<OutputConfig> outputs() const
        {
            std::vector<OutputConfig> outputs;
            outputs.reserve(_spec->schema.audio_outputs);
            for (size_t channel = 0; channel < _spec->schema.audio_outputs; ++channel) {
                outputs.push_back(OutputConfig{
                    .name = _spec->schema.audio_output_names[channel],
                });
            }
            return outputs;
        }

        template<class Allocator>
        void init_buffer(Allocator& allocator, InitBufferContext& context) const
        {
            State& state = allocator.template new_object<State>();
            auto spec_slot = context.template register_init_buffer<JuceVstWrapperSpec>(
                juce_details::make_juce_vst_spec_buffer_id(_spec->resource_id),
                1
            );
            spec_slot[0] = *_spec;
            auto slots = context.template use_tick_buffer<void*>(juce_details::make_juce_vst_runtime_resource_id(_spec->resource_id));
            allocator.assign(state.runtime_slot, slots.data());
        }

        void tick_block(BlockTickState const& state) const;
    };
#endif
}
