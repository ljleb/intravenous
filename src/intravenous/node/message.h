#pragma once

#include <intravenous/graph/builder/syntax.h>
#include <intravenous/module/configuration_argument.h>
#include <intravenous/node/address.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {
class NodeRef;

namespace details {
template<class T>
struct IsConfigurationJsonSequence : std::false_type {};

template<class T, class Allocator>
struct IsConfigurationJsonSequence<std::vector<T, Allocator>> : std::true_type {};

template<class T, std::size_t Size>
struct IsConfigurationJsonSequence<std::array<T, Size>> : std::true_type {};

[[nodiscard]] std::string configuration_json_boolean(bool value);
[[nodiscard]] std::string configuration_json_signed_integer(std::int64_t value);
[[nodiscard]] std::string configuration_json_unsigned_integer(std::uint64_t value);
[[nodiscard]] std::string configuration_json_floating_point(double value);
[[nodiscard]] std::string configuration_json_string(std::string_view value);
[[nodiscard]] std::string configuration_node_address_json(NodeAddress const& value);
[[nodiscard]] std::string configuration_schema_json_value(
    void const* data,
    ConfigurationExpressionTypeSchema const& schema);

template<class T>
[[nodiscard]] std::string configuration_json_value(T const& value)
{
    using Value = std::remove_cvref_t<T>;
    if constexpr (std::same_as<Value, bool>) {
        return configuration_json_boolean(value);
    } else if constexpr (std::is_enum_v<Value>) {
        auto const* identity = configuration_type_identity<Value>();
        auto const* schema = identity ? identity->expression_schema : nullptr;
        if (!schema || schema->kind != ConfigurationExpressionTypeKind::enumeration) {
            throw std::logic_error(
                "replacement enum has no generated configuration metadata");
        }
        return configuration_schema_json_value(std::addressof(value), *schema);
    } else if constexpr (std::integral<Value>) {
        if constexpr (std::is_signed_v<Value>) {
            return configuration_json_signed_integer(
                static_cast<std::int64_t>(value));
        } else {
            return configuration_json_unsigned_integer(
                static_cast<std::uint64_t>(value));
        }
    } else if constexpr (std::floating_point<Value>) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument(
                "replacement floating-point argument is not finite");
        }
        return configuration_json_floating_point(static_cast<double>(value));
    } else if constexpr (std::same_as<Value, std::string>
        || std::same_as<Value, std::string_view>) {
        return configuration_json_string(value);
    } else if constexpr (std::is_same_v<Value, char const*>
        || std::is_same_v<Value, char*>) {
        if (!value) {
            throw std::invalid_argument(
                "replacement string argument is null");
        }
        return configuration_json_string(value);
    } else if constexpr (std::is_array_v<Value>
        && std::same_as<std::remove_cv_t<std::remove_extent_t<Value>>, char>) {
        return configuration_json_string(std::string_view(value));
    } else if constexpr (std::same_as<Value, NodeAddress>) {
        return configuration_node_address_json(value);
    } else if constexpr (IsConfigurationJsonSequence<Value>::value) {
        std::string result{"["};
        bool first = true;
        for (auto const& element : value) {
            if (!first) result.push_back(',');
            result += configuration_json_value(element);
            first = false;
        }
        result.push_back(']');
        return result;
    } else {
        auto const* identity = configuration_type_identity<Value>();
        if (!identity || !identity->expression_schema) {
            throw std::logic_error(
                "ctx.replace() argument type has no generated Front 9 metadata");
        }
        return configuration_schema_json_value(
            std::addressof(value), *identity->expression_schema);
    }
}

template<class... Args>
[[nodiscard]] std::string configuration_arguments_json(Args const&... arguments)
{
    std::string result{"["};
    bool first = true;
    auto append = [&](auto const& argument) {
        if (!first) result.push_back(',');
        result += configuration_json_value(argument);
        first = false;
    };
    (append(arguments), ...);
    result.push_back(']');
    return result;
}
} // namespace details

class OwnedNodeMessage {
    std::shared_ptr<void const> owner_{};
    void const* data_ = nullptr;
    details::ConfigurationTypeIdentity const* type_ = nullptr;

public:
    OwnedNodeMessage() = default;

    template<class Event>
    explicit OwnedNodeMessage(Event&& event)
      requires std::constructible_from<
          std::remove_cvref_t<Event>, Event&&>
        : owner_(std::make_shared<std::remove_cvref_t<Event>>(
              std::forward<Event>(event)))
        , data_(owner_.get())
        , type_(details::configuration_type_identity<std::remove_cvref_t<Event>>())
    {}

    static OwnedNodeMessage from_owned(
        std::shared_ptr<void const> owner,
        void const* data,
        details::ConfigurationTypeIdentity const* type)
    {
        OwnedNodeMessage result;
        result.owner_ = std::move(owner);
        result.data_ = data;
        result.type_ = type;
        return result;
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return owner_ && data_ && type_;
    }

    [[nodiscard]] void const* data() const noexcept { return data_; }
    [[nodiscard]] details::ConfigurationTypeIdentity const* type() const noexcept
    {
        return type_;
    }
};

struct NodeMessageContextOperations {
    void* context = nullptr;
    void (*send)(void*, NodeAddress const&, OwnedNodeMessage) = nullptr;
    void (*replace)(
        void*, NodeAddress const&, std::string, std::string) = nullptr;
};

[[nodiscard]] NodeAddress node_message_target(
    NodeAddress const& context_address,
    NodeRef const& target);

class NodeMessageContextCore {
    NodeMessageContextOperations const* operations_ = nullptr;
    NodeAddress const* address_ = nullptr;

public:
    NodeMessageContextCore(
        NodeMessageContextOperations const& operations,
        NodeAddress const& address) noexcept
        : operations_(&operations), address_(&address)
    {}

    [[nodiscard]] NodeAddress const& address() const noexcept { return *address_; }

    template<class Event>
    void send(NodeAddress target, Event&& event) const
    {
        if (!operations_ || !operations_->send) {
            throw std::logic_error("node-message context cannot send events");
        }
        operations_->send(
            operations_->context,
            target,
            OwnedNodeMessage(std::forward<Event>(event)));
    }

    template<class Event>
    void send(NodeRef const& target, Event&& event) const
    {
        send(
            node_message_target(address(), target),
            std::forward<Event>(event));
    }

    // The handler supplies the successor definition and its complete typed
    // argument list. Package code serializes those values before the callback
    // returns. The runtime never recovers predecessor construction arguments.
    template<fixed_string DefinitionId, class... Args>
    void replace(Args&&... arguments) const
    {
        if (!operations_ || !operations_->replace) {
            throw std::logic_error("node-message context cannot replace its node");
        }
        operations_->replace(
            operations_->context,
            address(),
            std::string(DefinitionId.view()),
            details::configuration_arguments_json(arguments...));
    }
};
static_assert(std::is_standard_layout_v<NodeMessageContextCore>);
static_assert(sizeof(NodeMessageContextCore) == 2 * sizeof(void*));

template<class Node>
class NodeMessageContext final : public NodeMessageContextCore {
public:
    using NodeMessageContextCore::NodeMessageContextCore;
};
static_assert(sizeof(NodeMessageContext<void>) == sizeof(NodeMessageContextCore));

struct ModuleMessageLocalRef {
    std::string name{};
    std::vector<ProjectNodePathSelector> relative_path{};
    bool operator==(ModuleMessageLocalRef const&) const = default;
};

struct ModuleMessageLocalRefs {
    std::vector<ModuleMessageLocalRef> values{};
};

class ModuleMessageTarget {
    NodeAddress address_{};

public:
    explicit ModuleMessageTarget(NodeAddress address)
        : address_(std::move(address))
    {}

    [[nodiscard]] NodeAddress const& address() const noexcept { return address_; }

    [[nodiscard]] ModuleMessageTarget select(
        ProjectNodePathSelector selector) const
    {
        auto result = *this;
        result.address_.path.push_back(std::move(selector));
        return result;
    }

    [[nodiscard]] ModuleMessageTarget member(std::size_t index) const
    {
        return select(ProjectVirtualMemberSelector{.index = index});
    }

    template<fixed_string Name>
    [[nodiscard]] ModuleMessageTarget tiled_child() const
    {
        return select(ProjectTiledChildSelector{
            .name = std::string(Name.view()),
        });
    }
};

template<class Module>
class ModuleMessageContext final {
    NodeMessageContextCore core_;
    ModuleMessageLocalRefs const* local_refs_ = nullptr;

public:
    ModuleMessageContext(
        NodeMessageContextOperations const& operations,
        NodeAddress const& address,
        ModuleMessageLocalRefs const* local_refs) noexcept
        : core_(operations, address)
        , local_refs_(local_refs)
    {}

    [[nodiscard]] NodeAddress const& address() const noexcept
    {
        return core_.address();
    }

    template<fixed_string Name>
    [[nodiscard]] ModuleMessageTarget ref() const
    {
        if (!local_refs_) {
            throw std::logic_error(
                "module-message context has no local virtual-node references");
        }
        auto const name = Name.view();
        auto const matches = std::ranges::count_if(
            local_refs_->values,
            [&](ModuleMessageLocalRef const& candidate) {
                return candidate.name == name;
            });
        if (matches == 0) {
            throw std::logic_error(
                "module-message local virtual node '" + std::string(name)
                + "' was not created by build_graph()");
        }
        if (matches != 1) {
            throw std::logic_error(
                "module-message local virtual node '" + std::string(name)
                + "' is ambiguous");
        }
        auto const& local = *std::ranges::find_if(
            local_refs_->values,
            [&](ModuleMessageLocalRef const& candidate) {
                return candidate.name == name;
            });
        auto target = address();
        target.path.insert(
            target.path.end(),
            local.relative_path.begin(),
            local.relative_path.end());
        return ModuleMessageTarget(std::move(target));
    }

    template<class Event>
    void send(ModuleMessageTarget const& target, Event&& event) const
    {
        core_.send(target.address(), std::forward<Event>(event));
    }

    template<class Event>
    void send(NodeAddress target, Event&& event) const
    {
        core_.send(std::move(target), std::forward<Event>(event));
    }

    template<fixed_string DefinitionId, class... Args>
    void replace(Args&&... arguments) const
    {
        core_.template replace<DefinitionId>(
            std::forward<Args>(arguments)...);
    }
};
static_assert(std::is_standard_layout_v<ModuleMessageContext<void>>);
static_assert(
    sizeof(ModuleMessageContext<void>) == 3 * sizeof(void*));

using ErasedNodeMessageHandler = void (*)(
    void const*,
    NodeMessageContextOperations const&,
    NodeAddress const&,
    ModuleMessageLocalRefs const*,
    void const*);

struct ConfiguredNodeMessageHandler {
    std::vector<ProjectNodePathSelector> relative_path{};
    details::ConfigurationTypeIdentity const* event_type = nullptr;
    std::shared_ptr<void const> receiver{};
    std::shared_ptr<ModuleMessageLocalRefs const> module_local_refs{};
    ErasedNodeMessageHandler invoke = nullptr;
};

struct BoundNodeMessageHandler {
    NodeAddress address{};
    details::ConfigurationTypeIdentity const* event_type = nullptr;
    std::shared_ptr<void const> receiver{};
    std::shared_ptr<ModuleMessageLocalRefs const> module_local_refs{};
    ErasedNodeMessageHandler invoke = nullptr;
};
} // namespace iv
