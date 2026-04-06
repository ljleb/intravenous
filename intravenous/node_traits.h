#pragma once

#include "ports.h"

#include <concepts>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>

namespace iv {
    template<typename Node>
    struct NodeState {
        using Type = void;
    };

    namespace details {
        template<typename Node>
        concept has_State = requires {
            typename Node::State;
        };
    }

    template<typename Node>
    requires(details::has_State<Node>)
    struct NodeState<Node> {
        using Type = typename Node::State;
    };

    template<typename A>
    struct NoCopy : public A
    {
        NoCopy(NoCopy const&) = delete;
        NoCopy(NoCopy&&) = delete;
    };

    template<>
    struct NoCopy<void>
    {
        NoCopy(NoCopy const&) = delete;
        NoCopy(NoCopy&&) = delete;
    };

    namespace details
    {
        template <typename Node>
        concept has_outputs = requires(Node const& node)
        {
            std::begin(node.outputs());
            std::end(node.outputs());
        };

        template <typename Node>
        concept has_num_outputs = requires(Node node, size_t num_outputs)
        {
            num_outputs = node.num_outputs();
        };

        template <typename Node>
        concept has_inputs = requires(Node const& node)
        {
            std::begin(node.inputs());
            std::end(node.inputs());
        };

        template <typename Node>
        concept has_event_outputs = requires(Node const& node)
        {
            std::begin(node.event_outputs());
            std::end(node.event_outputs());
        };

        template <typename Node>
        concept has_num_event_outputs = requires(Node node, size_t num_outputs)
        {
            num_outputs = node.num_event_outputs();
        };

        template <typename Node>
        concept has_event_inputs = requires(Node const& node)
        {
            std::begin(node.event_inputs());
            std::end(node.event_inputs());
        };

        template <typename Node>
        concept has_num_event_inputs = requires(Node node, size_t num_inputs)
        {
            num_inputs = node.num_event_inputs();
        };

        template <typename Node>
        concept has_num_inputs = requires(Node node, size_t num_inputs)
        {
            num_inputs = node.num_inputs();
        };

        template <typename Node>
        concept has_internal_latency = requires(Node node, size_t internal_latency)
        {
            internal_latency = node.internal_latency();
        };

        template <typename Node>
        concept has_max_block_size_method = requires(Node node, size_t block_size)
        {
            block_size = node.max_block_size();
        };

        template <typename Node>
        concept has_ttl_method = requires(Node node, std::optional<size_t> ttl)
        {
            ttl = node.ttl_samples();
        };

    }

    template<typename Node>
    constexpr auto get_outputs(Node const& node)
    {
        if constexpr (details::has_outputs<Node>)
        {
            return node.outputs();
        }
        else
        {
            return std::span<OutputConfig, 0>{};
        }
    }

    template<typename Node>
    constexpr auto get_num_outputs(Node const& node)
    {
        if constexpr (details::has_num_outputs<Node>)
        {
            return node.num_outputs();
        }
        else
        {
            return get_outputs(node).size();
        }
    }

    template<typename Node>
    constexpr auto get_inputs(Node const& node)
    {
        if constexpr (details::has_inputs<Node>)
        {
            return node.inputs();
        }
        else
        {
            return std::span<InputConfig, 0>{};
        }
    }

    template<typename Node>
    constexpr auto get_num_inputs(Node const& node)
    {
        if constexpr (details::has_num_inputs<Node>)
        {
            return node.num_inputs();
        }
        else
        {
            return get_inputs(node).size();
        }
    }

    template<typename Node>
    constexpr auto get_event_outputs(Node const& node)
    {
        if constexpr (details::has_event_outputs<Node>)
        {
            return node.event_outputs();
        }
        else
        {
            return std::span<EventOutputConfig, 0>{};
        }
    }

    template<typename Node>
    constexpr auto get_num_event_outputs(Node const& node)
    {
        if constexpr (details::has_num_event_outputs<Node>)
        {
            return node.num_event_outputs();
        }
        else
        {
            return get_event_outputs(node).size();
        }
    }

    template<typename Node>
    constexpr auto get_event_inputs(Node const& node)
    {
        if constexpr (details::has_event_inputs<Node>)
        {
            return node.event_inputs();
        }
        else
        {
            return std::span<EventInputConfig, 0>{};
        }
    }

    template<typename Node>
    constexpr auto get_num_event_inputs(Node const& node)
    {
        if constexpr (details::has_num_event_inputs<Node>)
        {
            return node.num_event_inputs();
        }
        else
        {
            return get_event_inputs(node).size();
        }
    }

    template<typename Node>
    constexpr size_t get_internal_latency(Node const& node)
    {
        if constexpr (details::has_internal_latency<Node>)
        {
            return node.internal_latency();
        }
        else
        {
            return 0;
        }
    }

    template<typename Node>
    constexpr size_t get_max_block_size(Node const& node)
    {
        if constexpr (details::has_max_block_size_method<Node>)
        {
            return node.max_block_size();
        }
        else
        {
            return MAX_BLOCK_SIZE;
        }
    }

    template<typename Node>
    constexpr std::optional<size_t> get_ttl_samples(Node const& node)
    {
        if constexpr (details::has_ttl_method<Node>)
        {
            return node.ttl_samples();
        }
        else
        {
            return std::nullopt;
        }
    }

    template <typename Node>
    void info(Node&& node)
    {
        std::cout << "internal latency: " << get_internal_latency(node) << "\n";

        auto const block_size = get_max_block_size(node);
        std::cout << "max block size: " << ((block_size == MAX_BLOCK_SIZE) ? std::string("unbounded") : std::to_string(block_size)) << "\n";
        std::cout << "params:\n";

        for (auto const& in : get_inputs(node)) {
            std::cout << "    in: " << in.name << " (" << in.default_value << ")\n";
        }
        for (auto const& out : get_outputs(node)) {
            std::cout << "    out: " << out.name << "\n";
        }
    }
}
