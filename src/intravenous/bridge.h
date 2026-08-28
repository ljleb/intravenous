#pragma once

#include <intravenous/linker_event.h>

#include <concepts>
#include <functional>
#include <stdexcept>
#include <utility>

// Declare a bridge's two concrete participants and its move-only binding
// scope. Define it exactly once with IV_DEFINE_BRIDGE in the matching .cpp.
#define IV_DECLARE_BRIDGE(bridge_type, left_participant, right_participant) \
    struct bridge_type {                                                    \
        using left_type = left_participant;                                 \
        using right_type = right_participant;                               \
                                                                            \
        class scope {                                                       \
        public:                                                             \
            scope(left_type* left, right_type* right);                      \
            scope(left_type& left, right_type& right);                      \
            ~scope();                                                       \
            scope(scope const&) = delete;                                   \
            scope& operator=(scope const&) = delete;                        \
            scope(scope&& other) noexcept;                                  \
            scope& operator=(scope&& other) noexcept;                       \
                                                                            \
        private:                                                            \
            void clear() noexcept;                                          \
            bool active_ = false;                                           \
        };                                                                  \
                                                                            \
        static scope bind(left_type* left, right_type* right);              \
        static scope bind(left_type& left, right_type& right);              \
                                                                            \
        template <class T>                                                  \
        static T* get() noexcept                                            \
        {                                                                   \
            static_assert(                                                  \
                std::same_as<T, left_type> || std::same_as<T, right_type>,  \
                "bridge::get<T>() requires a bridge participant");        \
            if constexpr (std::same_as<T, left_type>) {                     \
                return left_;                                               \
            } else {                                                        \
                return right_;                                              \
            }                                                               \
        }                                                                   \
                                                                            \
    private:                                                                \
        static left_type* left_;                                            \
        static right_type* right_;                                          \
    }

// Emit a bridge's one process-global pair of participant slots and all scope
// operations. A second definition fails at link time; no definition leaves
// bind() unresolved.
#define IV_DEFINE_BRIDGE(bridge_type)                                       \
    bridge_type::left_type* bridge_type::left_ = nullptr;                   \
    bridge_type::right_type* bridge_type::right_ = nullptr;                 \
    bridge_type::scope::scope(                                              \
        bridge_type::left_type* left,                                       \
        bridge_type::right_type* right)                                     \
    {                                                                       \
        if (left == nullptr || right == nullptr) {                          \
            throw std::invalid_argument("bridge participants must not be null"); \
        }                                                                   \
        if (bridge_type::left_ != nullptr || bridge_type::right_ != nullptr) { \
            throw std::logic_error("bridge is already bound");             \
        }                                                                   \
        bridge_type::left_ = left;                                          \
        bridge_type::right_ = right;                                        \
        active_ = true;                                                     \
    }                                                                       \
    bridge_type::scope::scope(                                              \
        bridge_type::left_type& left,                                       \
        bridge_type::right_type& right)                                     \
        : scope(&left, &right)                                              \
    {}                                                                      \
    bridge_type::scope::~scope()                                            \
    {                                                                       \
        clear();                                                            \
    }                                                                       \
    bridge_type::scope::scope(scope&& other) noexcept                       \
        : active_(std::exchange(other.active_, false))                      \
    {}                                                                      \
    bridge_type::scope& bridge_type::scope::operator=(scope&& other) noexcept \
    {                                                                       \
        if (this != &other) {                                               \
            clear();                                                        \
            active_ = std::exchange(other.active_, false);                  \
        }                                                                   \
        return *this;                                                       \
    }                                                                       \
    void bridge_type::scope::clear() noexcept                               \
    {                                                                       \
        if (!std::exchange(active_, false)) {                               \
            return;                                                         \
        }                                                                   \
        bridge_type::left_ = nullptr;                                       \
        bridge_type::right_ = nullptr;                                      \
    }                                                                       \
    bridge_type::scope bridge_type::bind(                                   \
        bridge_type::left_type* left,                                       \
        bridge_type::right_type* right)                                     \
    {                                                                       \
        return scope(left, right);                                          \
    }                                                                       \
    bridge_type::scope bridge_type::bind(                                   \
        bridge_type::left_type& left,                                       \
        bridge_type::right_type& right)                                     \
    {                                                                       \
        return scope(left, right);                                          \
    }

template <class Bridge, auto Member>
struct iv_bridge_subscriber;

template <class Bridge, class C, class... Parameters,
          void (C::*Member)(Parameters...)>
struct iv_bridge_subscriber<Bridge, Member> {
    static void invoke(Parameters... parameters)
    {
        static_assert(
            std::same_as<C, typename Bridge::left_type>
                || std::same_as<C, typename Bridge::right_type>,
            "bridge subscriber owner must be one of the bridge's participants");
        if (auto* instance = Bridge::template get<C>()) {
            std::invoke(Member, *instance, std::forward<Parameters>(parameters)...);
        }
    }
};

template <class Bridge, class C, class... Parameters,
          void (C::*Member)(Parameters...) const>
struct iv_bridge_subscriber<Bridge, Member> {
    static void invoke(Parameters... parameters)
    {
        static_assert(
            std::same_as<C, typename Bridge::left_type>
                || std::same_as<C, typename Bridge::right_type>,
            "bridge subscriber owner must be one of the bridge's participants");
        if (auto* instance = Bridge::template get<C>()) {
            std::invoke(Member, *instance, std::forward<Parameters>(parameters)...);
        }
    }
};

#define IV_SUBSCRIBE_LINKER_EVENT(bridge_type, event_name, member)                \
    namespace {                                                              \
        IV_LINKER_EVENT_USED                                                 \
        IV_LINKER_EVENT_SECTION(event_name)                                  \
        event_name##_subscriber_type const                                   \
        IV_LINKER_EVENT_CONCAT(iv_bridge_subscriber_, __COUNTER__) =         \
            &::iv_bridge_subscriber<bridge_type, member>::invoke;           \
    }

#define IV_SUBSCRIBE_SINGLETON_BRIDGE(bridge_type, event_name, member)      \
    extern "C" {                                                            \
    event_name##_subscriber_type event_name =                                \
        &::iv_bridge_subscriber<bridge_type, member>::invoke;                \
    }
