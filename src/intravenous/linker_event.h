#pragma once

#include <span>
#include <utility>

#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

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

namespace iv::details {
struct LinkerEventTraceFrame {
    enum class Kind {
        event,
        module,
    };

    Kind kind = Kind::event;
    std::string name{};
    std::source_location location{};
};

struct LinkerEventModuleVisit {
    std::string module_name{};
    std::vector<LinkerEventTraceFrame> path{};
};

struct LinkerEventPropagationContext {
    std::string root_event{};
    std::source_location root_location{};
    std::unordered_map<void const*, LinkerEventModuleVisit> visits{};
    std::vector<LinkerEventTraceFrame> path{};
};

inline thread_local std::size_t linker_event_depth = 0;
inline thread_local LinkerEventPropagationContext* current_linker_event_propagation = nullptr;

template<class T>
inline constexpr unsigned char linker_event_module_identity = 0;

template<class T>
constexpr std::string_view linker_event_type_name()
{
#if defined(__clang__)
    constexpr std::string_view pretty = __PRETTY_FUNCTION__;
    constexpr std::string_view prefix = "T = ";
    auto const begin = pretty.find(prefix) + prefix.size();
    auto const end = pretty.find(']', begin);
    return pretty.substr(begin, end - begin);
#elif defined(__GNUC__)
    constexpr std::string_view pretty = __PRETTY_FUNCTION__;
    constexpr std::string_view prefix = "with T = ";
    auto const begin = pretty.find(prefix) + prefix.size();
    auto end = pretty.find(';', begin);
    if (end == std::string_view::npos) {
        end = pretty.find(']', begin);
    }
    return pretty.substr(begin, end - begin);
#else
    return "application module";
#endif
}

inline std::string format_linker_event_path(
    std::vector<LinkerEventTraceFrame> const& path)
{
    std::ostringstream out;
    for (auto const& frame : path) {
        out << "    " << (frame.kind == LinkerEventTraceFrame::Kind::event ? "event " : "module ")
            << frame.name;
        if (frame.kind == LinkerEventTraceFrame::Kind::event
            && frame.location.line() != 0) {
            out << " (" << frame.location.file_name() << ':' << frame.location.line() << ')';
        }
        out << '\n';
    }
    return out.str();
}

class LinkerEventFrameScope {
    LinkerEventPropagationContext* context_ = nullptr;
    bool active_ = false;

public:
    LinkerEventFrameScope(
        std::string_view event_name,
        std::source_location location)
        : context_(current_linker_event_propagation)
    {
        if (context_ != nullptr) {
            context_->path.push_back(LinkerEventTraceFrame{
                .kind = LinkerEventTraceFrame::Kind::event,
                .name = std::string(event_name),
                .location = location,
            });
            active_ = true;
        }
        ++linker_event_depth;
    }

    ~LinkerEventFrameScope()
    {
        if (active_) {
            context_->path.pop_back();
        }
        --linker_event_depth;
    }

    LinkerEventFrameScope(LinkerEventFrameScope const&) = delete;
    LinkerEventFrameScope& operator=(LinkerEventFrameScope const&) = delete;
};

class LinkerEventModuleFrameScope {
    LinkerEventPropagationContext* context_ = nullptr;
    bool active_ = false;

public:
    template<class Module>
    explicit LinkerEventModuleFrameScope(std::type_identity<Module>)
        : context_(current_linker_event_propagation)
    {
        if (context_ == nullptr) {
            return;
        }
        auto* context = context_;

        auto const module_id = static_cast<void const*>(
            &linker_event_module_identity<Module>);
        auto const module_name = std::string(linker_event_type_name<Module>());
        auto current_path = context->path;
        current_path.push_back(LinkerEventTraceFrame{
            .kind = LinkerEventTraceFrame::Kind::module,
            .name = module_name,
            .location = {},
        });

        if (auto const first = context->visits.find(module_id);
            first != context->visits.end()) {
            std::ostringstream message;
            message << "event propagation re-entered module " << module_name << '\n'
                    << "root event: " << context->root_event;
            if (context->root_location.line() != 0) {
                message << " (" << context->root_location.file_name()
                        << ':' << context->root_location.line() << ')';
            }
            message << "\nfirst path:\n"
                    << format_linker_event_path(first->second.path)
                    << "second path:\n"
                    << format_linker_event_path(current_path);
            throw std::logic_error(message.str());
        }

        context->visits.emplace(module_id, LinkerEventModuleVisit{
            .module_name = module_name,
            .path = current_path,
        });
        context->path.push_back(LinkerEventTraceFrame{
            .kind = LinkerEventTraceFrame::Kind::module,
            .name = std::move(module_name),
            .location = {},
        });
        active_ = true;
    }

    ~LinkerEventModuleFrameScope()
    {
        if (active_) {
            context_->path.pop_back();
        }
    }

    LinkerEventModuleFrameScope(LinkerEventModuleFrameScope const&) = delete;
    LinkerEventModuleFrameScope& operator=(LinkerEventModuleFrameScope const&) = delete;
};

class LinkerEventPropagationScope {
    LinkerEventPropagationContext* previous_ = nullptr;

public:
    explicit LinkerEventPropagationScope(LinkerEventPropagationContext& context)
        : previous_(std::exchange(current_linker_event_propagation, &context))
    {}

    ~LinkerEventPropagationScope()
    {
        current_linker_event_propagation = previous_;
    }

    LinkerEventPropagationScope(LinkerEventPropagationScope const&) = delete;
    LinkerEventPropagationScope& operator=(LinkerEventPropagationScope const&) = delete;
};

template<class Dispatch>
decltype(auto) invoke_linker_event(
    std::string_view event_name,
    std::source_location location,
    bool source,
    Dispatch&& dispatch)
{
    if (!source) {
        LinkerEventFrameScope frame(event_name, location);
        return std::forward<Dispatch>(dispatch)();
    }

    if (linker_event_depth != 0 || current_linker_event_propagation != nullptr) {
        throw std::logic_error(
            "linker event source '" + std::string(event_name)
            + "' was raised from inside an existing event propagation");
    }

    LinkerEventPropagationContext context{
        .root_event = std::string(event_name),
        .root_location = location,
    };
    LinkerEventPropagationScope propagation_scope(context);
    LinkerEventFrameScope frame(event_name, location);
    return std::forward<Dispatch>(dispatch)();
}

template<class Module, class Dispatch>
decltype(auto) invoke_linker_event_module(Dispatch&& dispatch)
{
    LinkerEventModuleFrameScope frame(std::type_identity<Module>{});
    return std::forward<Dispatch>(dispatch)();
}
} // namespace iv::details

#define IV_DECLARE_LINKER_EVENT(event_type, event_name) \
    using event_name##_subscriber_type = event_type; \
    std::span<event_type const> event_name##_subscribers()

#define IV_DECLARE_SINGLETON_EVENT(event_type, event_name) \
    using event_name##_subscriber_type = event_type; \
    extern "C" event_type event_name; \
    inline event_type event_name##_subscriber() { return event_name; }

#define IV_DEFINE_SINGLETON_EVENT(event_type, event_name, default_value) \
    extern "C" { \
    IV_LINKER_EVENT_WEAK event_type event_name = (default_value); \
    }

#define IV_SUBSCRIBE_SINGLETON_EVENT(event_type, event_name, value) \
    extern "C" { \
    event_type event_name = (value); \
    }

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

#define IV_LINKER_EVENT_LOCATION std::source_location::current()

#define IV_INVOKE_LINKER_EVENT_IMPL(source_, event_name, ...) \
    do { \
        auto iv_linker_event_invoke = [&](auto&&... iv_linker_event_args) { \
            ::iv::details::invoke_linker_event( \
                #event_name, \
                IV_LINKER_EVENT_LOCATION, \
                (source_), \
                [&] { \
                    for (auto const subscriber : event_name##_subscribers()) { \
                        subscriber(iv_linker_event_args...); \
                    } \
                }); \
        }; \
        iv_linker_event_invoke(__VA_ARGS__); \
    } while (false)

#define IV_INVOKE_LINKER_EVENT(event_name, ...) \
    IV_INVOKE_LINKER_EVENT_IMPL(false, event_name __VA_OPT__(,) __VA_ARGS__)

#define IV_INVOKE_LINKER_EVENT_SOURCE(event_name, ...) \
    IV_INVOKE_LINKER_EVENT_IMPL(true, event_name __VA_OPT__(,) __VA_ARGS__)

#define IV_INVOKE_SINGLETON_EVENT_IMPL(source_, event_name, ...) \
    do { \
        auto iv_linker_event_invoke = [&](auto&&... iv_linker_event_args) { \
            ::iv::details::invoke_linker_event( \
                #event_name, \
                IV_LINKER_EVENT_LOCATION, \
                (source_), \
                [&] { event_name##_subscriber()(iv_linker_event_args...); }); \
        }; \
        iv_linker_event_invoke(__VA_ARGS__); \
    } while (false)

#define IV_INVOKE_SINGLETON_EVENT(event_name, ...) \
    IV_INVOKE_SINGLETON_EVENT_IMPL(false, event_name __VA_OPT__(,) __VA_ARGS__)

#define IV_INVOKE_SINGLETON_EVENT_SOURCE(event_name, ...) \
    IV_INVOKE_SINGLETON_EVENT_IMPL(true, event_name __VA_OPT__(,) __VA_ARGS__)
