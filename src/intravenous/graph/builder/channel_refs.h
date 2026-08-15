#pragma once

#include <intravenous/channel_ports.h>
#include <intravenous/graph/builder/port_refs.h>

#include <array>
#include <cstddef>

namespace iv {
    template<class Type>
    class ChannelRefs {
        std::array<SamplePortRef, Type::channel_count> refs_ {};

    public:
        template<class Tag>
        SamplePortRef &operator[](ChannelPortId<Type, Tag>)
        {
            return refs_[ChannelMember<Type, Tag>::channel_ordinal];
        }

        template<class Tag>
        SamplePortRef const &operator[](ChannelPortId<Type, Tag>) const
        {
            return refs_[ChannelMember<Type, Tag>::channel_ordinal];
        }
    };
}
