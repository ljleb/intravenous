#pragma once

#include <span>
#include <utility>

#if defined(__GNUC__) || defined(__clang__)
#define IV_LINKER_EVENT_USED __attribute__((used))
#define IV_LINKER_EVENT_SECTION(name) __attribute__((section(#name)))
#define IV_LINKER_EVENT_WEAK __attribute__((weak))
#else
#define IV_LINKER_EVENT_USED
#define IV_LINKER_EVENT_SECTION(name)
#define IV_LINKER_EVENT_WEAK
#endif

#define IV_LINKER_EVENT_CONCAT_INNER(a, b) a##b
#define IV_LINKER_EVENT_CONCAT(a, b) IV_LINKER_EVENT_CONCAT_INNER(a, b)

#define IV_DECLARE_LINKER_EVENT(event_type, event_name) \
    std::span<event_type const> event_name##_subscribers()

#define IV_DECLARE_SINGLETON_EVENT(event_type, event_name) \
    extern "C" event_type event_name; \
    inline event_type event_name##_subscriber() { return event_name; }

#define IV_DEFINE_SINGLETON_EVENT(event_type, event_name, default_value) \
    extern "C" IV_LINKER_EVENT_WEAK event_type event_name = (default_value)

#define IV_SUBSCRIBE_SINGLETON_EVENT(event_type, event_name, value) \
    extern "C" event_type event_name = (value)

#define IV_DEFINE_LINKER_EVENT(event_type, event_name) \
    extern "C" { \
    extern event_type const IV_LINKER_EVENT_CONCAT(__start_, event_name)[] \
        IV_LINKER_EVENT_WEAK; \
    extern event_type const IV_LINKER_EVENT_CONCAT(__stop_, event_name)[] \
        IV_LINKER_EVENT_WEAK; \
    } \
    std::span<event_type const> event_name##_subscribers() { \
        auto const *begin = IV_LINKER_EVENT_CONCAT(__start_, event_name); \
        auto const *end = IV_LINKER_EVENT_CONCAT(__stop_, event_name); \
        if (begin == nullptr || end == nullptr) { \
            return {}; \
        } \
        return {begin, end}; \
    }

#define IV_SUBSCRIBE_LINKER_EVENT(event_type, event_name, ...) \
    namespace { \
        alignas(event_type) IV_LINKER_EVENT_USED \
        IV_LINKER_EVENT_SECTION(event_name) event_type const \
        IV_LINKER_EVENT_CONCAT(iv_linker_event_subscriber_, __COUNTER__){__VA_ARGS__}; \
    }

#define IV_INVOKE_LINKER_EVENT(event_name, ...) \
    do { \
        auto iv_linker_event_invoke = [&](auto&&... iv_linker_event_args) { \
            for (auto const subscriber : event_name##_subscribers()) { \
                subscriber(iv_linker_event_args...); \
            } \
        }; \
        iv_linker_event_invoke(__VA_ARGS__); \
    } while (false)

#define IV_INVOKE_SINGLETON_EVENT(event_name, ...) \
    do { \
        event_name##_subscriber()(__VA_ARGS__); \
    } while (false)
