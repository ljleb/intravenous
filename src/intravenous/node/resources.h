#pragma once

#include <memory>
#include <stdexcept>

namespace iv {
    struct JuceVstWrapperSpec;

    using UniqueResource = std::unique_ptr<void, void(*)(void*)>;

    struct ResourceContext {
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
    };
}
