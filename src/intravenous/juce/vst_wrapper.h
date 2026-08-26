#pragma once

#include <intravenous/graph/builder.h>
#include <intravenous/juce/vst_types.h>

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
