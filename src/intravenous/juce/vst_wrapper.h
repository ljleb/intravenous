#pragma once

#include <intravenous/graph/builder.h>
#include <intravenous/node/lifecycle.h>

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
    namespace details {
        template<class...>
        inline constexpr bool dependent_false_v = false;
    }

    namespace juce {
#if !IV_ENABLE_JUCE_VST
        template<typename... Args>
        auto vst(Args&&...)
        {
            static_assert(
                details::dependent_false_v<Args...>,
                "iv::juce::vst(...) requires JUCE VST support. Configure the project with JUCE available so IV_ENABLE_JUCE_VST=1."
            );
            return NodeRef();
        }
#endif
    }

#if IV_ENABLE_JUCE_VST
    struct JuceVstParameterSpec {
        std::string id;
        std::string name;
        Sample default_value = 0.0f;
        Sample min = 0.0f;
        Sample max = 1.0f;
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

    struct JuceVstWrapperSpec {
        JuceVstPluginConfig plugin;
        JuceVstSchema schema;
    };

    JuceVstWrapperSpec probe_juce_vst(JuceVstPluginConfig request);

    namespace juce {
        template<typename... Args>
        auto vst(Args&&...)
        {
            static_assert(
                details::dependent_false_v<Args...>,
                "iv::juce::vst(...) is temporarily unavailable while VST discovery-generated node types are introduced."
            );
            return NodeRef();
        }
    }
#endif
}
