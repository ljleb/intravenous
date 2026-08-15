#pragma once

#include <intravenous/channel_type.h>
#include <intravenous/graph/builder/syntax.h>

#include <concepts>
#include <type_traits>
#include <utility>

namespace iv {
    template<class Type, class Tag>
    using ChannelPortId = ChannelMember<Type, Tag>;

    template<class Member, class T>
    constexpr auto make_channel_assignment(Member, T&& value)
    {
        using MemberT = std::remove_cvref_t<Member>;
        return DefaultChannelNamedArg<
            typename MemberT::channel_type,
            MemberT::channel_ordinal,
            std::remove_cvref_t<T>>{
            .value = std::forward<T>(value),
        };
    }

    template<class Type, class Fn>
    constexpr void for_each_channel_port(Fn&& fn)
    {
        []<class... Members>(ChannelMemberList<Members...>, Fn&& callback) {
            (callback.template operator()<Members{}>(), ...);
        }(typename Type::members{}, std::forward<Fn>(fn));
    }

    template<class Type, class Tag>
    constexpr size_t port_index(ChannelMember<Type, Tag>)
    {
        return ChannelMember<Type, Tag>::channel_ordinal;
    }

    template<class Type, class Tag>
    constexpr auto swap_side(ChannelMember<Type, Tag>)
    {
        if constexpr (std::same_as<Type, stereo> && std::same_as<Tag, stereo::left_tag>) {
            return stereo::right;
        } else if constexpr (std::same_as<Type, stereo> && std::same_as<Tag, stereo::right_tag>) {
            return stereo::left;
        } else {
            return ChannelMember<Type, Tag>{};
        }
    }

}
