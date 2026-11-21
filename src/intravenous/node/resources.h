#pragma once

#include <intravenous/devices/system_audio_binding.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace iv {
    struct JuceVstWrapperSpec;
    using UniqueResource = std::unique_ptr<void, void(*)(void*)>;

    struct ResourceContext {
        struct SystemAudioResources {
            void* owner = nullptr;
            SystemAudioInputBinding (*resolve_input_fn)(
                void*, std::string_view) = nullptr;
            SystemAudioOutputBinding (*resolve_output_fn)(
                void*, std::string_view) = nullptr;

            SystemAudioInputBinding resolve_input(
                std::string_view device_id) const
            {
                return resolve_input_fn
                    ? resolve_input_fn(owner, device_id)
                    : SystemAudioInputBinding{};
            }

            SystemAudioOutputBinding resolve_output(
                std::string_view device_id) const
            {
                return resolve_output_fn
                    ? resolve_output_fn(owner, device_id)
                    : SystemAudioOutputBinding{};
            }
        };

        struct VstResources {
            void* owner = nullptr;
            UniqueResource (*create_juce_vst_fn)(void*, JuceVstWrapperSpec const&) = nullptr;

            UniqueResource create(JuceVstWrapperSpec const& descriptor) const
            {
                if (!create_juce_vst_fn) {
                    throw std::logic_error("VST resource callback is unavailable");
                }
                if (!owner) {
                    throw std::logic_error("VST resource owner is null");
                }
                return create_juce_vst_fn(owner, descriptor);
            }
        };

        VstResources vst {};
        SystemAudioResources system_audio {};
    };
}
