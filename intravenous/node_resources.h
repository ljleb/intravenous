#pragma once

#include <memory>
#include <stdexcept>

namespace iv {
    struct JuceVstWrapperSpec;
    class EventStreamStorage;

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
        EventStreamStorage* event_streams = nullptr;

        EventStreamStorage& event_stream_storage() const
        {
            if (!event_streams) {
                throw std::logic_error("event stream storage is unavailable");
            }
            return *event_streams;
        }
    };
}
