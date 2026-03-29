#pragma once

#include <memory>

namespace iv {
    struct JuceVstWrapperSpec;

    using UniqueResource = std::unique_ptr<void, void(*)(void*)>;

    struct ResourceContext {
        struct VstResources {
            void* owner = nullptr;
            UniqueResource (*create_juce_vst_fn)(void*, JuceVstWrapperSpec const&) = nullptr;

            UniqueResource create(JuceVstWrapperSpec const& descriptor) const
            {
                return create_juce_vst_fn
                    ? create_juce_vst_fn(owner, descriptor)
                    : UniqueResource(nullptr, +[](void*) {});
            }
        };

        VstResources vst {};
    };
}
