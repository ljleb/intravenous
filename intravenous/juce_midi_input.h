#pragma once

#include "dsl.h"
#include "node_lifecycle.h"

#include <array>
#include <memory>
#include <string>

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
        SampleNodeRef midi_input(Args&&...)
        {
            static_assert(
                details::dependent_false_v<Args...>,
                "iv::juce::midi_input(...) requires JUCE support. Configure the project with JUCE available so IV_ENABLE_JUCE_VST=1."
            );
            return SampleNodeRef();
        }
#endif
    }

#if IV_ENABLE_JUCE_VST
    struct JuceMidiInputSpec {
        std::string device_query;
    };

    class JuceMidiInputSource {
        std::shared_ptr<JuceMidiInputSpec const> _spec;

    public:
        struct State {
            UniqueResource live_input { nullptr, +[](void*) {} };
        };

        explicit JuceMidiInputSource(JuceMidiInputSpec spec) :
            _spec(std::make_shared<JuceMidiInputSpec const>(std::move(spec)))
        {}

        auto event_outputs() const
        {
            return std::array<EventOutputConfig, 1> {{
                { .name = "midi", .type = EventTypeId::midi }
            }};
        }

        void initialize(InitializationContext<JuceMidiInputSource> const& ctx) const
        {
            auto& state = ctx.state();
            state.live_input = ctx.resources.midi_input.create(*_spec);
        }

        void tick_block(TickBlockContext<JuceMidiInputSource> const& ctx) const;
    };

    namespace juce {
        inline SampleNodeRef midi_input(
            GraphBuilder& g,
            std::string device_query = {}
        )
        {
            return g.node<JuceMidiInputSource>(JuceMidiInputSpec {
                .device_query = std::move(device_query),
            });
        }
    }
#endif
}
