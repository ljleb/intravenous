#pragma once

#include <intravenous/channel_ports.h>
#include <intravenous/graph/builder/port_refs.h>

#include <array>
#include <cstddef>

namespace iv {
    template<ChannelTypeId Type>
    class ChannelRefs {
        static_assert(Type != ChannelTypeId::count);

        std::array<SamplePortRef, channel_count(Type)> refs_ {};

        template<auto Tag, size_t Index>
        static consteval size_t channel_slot()
        {
            (void)Index;
            return ChannelPortTraits<Type>::template channel_ordinal<Tag>();
        }

    public:
        template<auto Tag, size_t Index>
        SamplePortRef &operator[](ChannelPortId<Type, Tag, Index>)
        {
            return refs_[channel_slot<Tag, Index>()];
        }

        template<auto Tag, size_t Index>
        SamplePortRef const &operator[](ChannelPortId<Type, Tag, Index>) const
        {
            return refs_[channel_slot<Tag, Index>()];
        }
    };
}
