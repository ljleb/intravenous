#pragma once

#include <atomic>
#include <cstdint>
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
        source,
        event,
        module,
    };

    Kind kind = Kind::event;
    std::string name{};
    // The debugging name of an event source, or "transaction" for a module
    // frame that continues an earlier entry of that module.
    std::string detail{};
    std::source_location location{};
    // The module identity of a source or module frame.
    void const* identity = nullptr;
};

struct LinkerEventModuleVisit {
    std::string module_name{};
    std::vector<LinkerEventTraceFrame> path{};
};

// One propagation tree: everything raised inside one IV_EVENT_SOURCE scope.
struct LinkerEventPropagationContext {
    std::uint64_t serial = 0;
    LinkerEventTraceFrame source{};
    std::unordered_map<void const*, LinkerEventModuleVisit> visits{};
    std::vector<LinkerEventTraceFrame> path{};
};

inline thread_local LinkerEventPropagationContext* current_linker_event_propagation = nullptr;
inline std::atomic<std::uint64_t> next_linker_event_propagation_serial{1};

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

inline std::string format_linker_event_frame(LinkerEventTraceFrame const& frame)
{
    std::ostringstream out;
    switch (frame.kind) {
    case LinkerEventTraceFrame::Kind::source:
        out << "source ";
        break;
    case LinkerEventTraceFrame::Kind::event:
        out << "event ";
        break;
    case LinkerEventTraceFrame::Kind::module:
        out << "module ";
        break;
    }
    out << frame.name;
    if (!frame.detail.empty()) {
        out << (frame.kind == LinkerEventTraceFrame::Kind::module ? " (" : " \"")
            << frame.detail
            << (frame.kind == LinkerEventTraceFrame::Kind::module ? ")" : "\"");
    }
    if (frame.kind != LinkerEventTraceFrame::Kind::module && frame.location.line() != 0) {
        out << " (" << frame.location.file_name() << ':' << frame.location.line() << ')';
    }
    return out.str();
}

inline std::string format_linker_event_path(
    std::vector<LinkerEventTraceFrame> const& path)
{
    std::string out;
    for (auto const& frame : path) {
        out += "    ";
        out += format_linker_event_frame(frame);
        out += '\n';
    }
    return out;
}

class LinkerEventFrameScope {
    LinkerEventPropagationContext& context_;

public:
    LinkerEventFrameScope(
        LinkerEventPropagationContext& context,
        std::string_view event_name,
        std::source_location location)
        : context_(context)
    {
        context_.path.push_back(LinkerEventTraceFrame{
            .kind = LinkerEventTraceFrame::Kind::event,
            .name = std::string(event_name),
            .location = location,
        });
    }

    ~LinkerEventFrameScope() { context_.path.pop_back(); }

    LinkerEventFrameScope(LinkerEventFrameScope const&) = delete;
    LinkerEventFrameScope& operator=(LinkerEventFrameScope const&) = delete;
};

class LinkerEventModuleFrameScope {
    LinkerEventPropagationContext* context_ = nullptr;

public:
    template<class Module>
    explicit LinkerEventModuleFrameScope(std::type_identity<Module>)
        : context_(current_linker_event_propagation)
    {
        // A subscriber called directly rather than through an event.
        if (context_ == nullptr) {
            return;
        }
        auto* context = context_;

        auto const module_id = static_cast<void const*>(
            &linker_event_module_identity<Module>);
        LinkerEventTraceFrame frame{
            .kind = LinkerEventTraceFrame::Kind::module,
            .name = std::string(linker_event_type_name<Module>()),
            .identity = module_id,
        };
        auto current_path = context->path;
        current_path.push_back(frame);

        if (auto const first = context->visits.find(module_id);
            first != context->visits.end()) {
            std::ostringstream message;
            message << "event propagation re-entered module " << frame.name << '\n'
                    << "event " << format_linker_event_frame(context->source)
                    << "\nfirst path:\n"
                    << format_linker_event_path(first->second.path)
                    << "second path:\n"
                    << format_linker_event_path(current_path);
            throw std::logic_error(message.str());
        }

        context->visits.emplace(module_id, LinkerEventModuleVisit{
            .module_name = frame.name,
            .path = std::move(current_path),
        });
        context->path.push_back(std::move(frame));
    }

    ~LinkerEventModuleFrameScope()
    {
        if (context_ != nullptr) {
            context_->path.pop_back();
        }
    }

    LinkerEventModuleFrameScope(LinkerEventModuleFrameScope const&) = delete;
    LinkerEventModuleFrameScope& operator=(LinkerEventModuleFrameScope const&) = delete;
};

// Owns the propagation context of one event source entry. See IV_EVENT_SOURCE.
class LinkerEventSourceScope {
    LinkerEventPropagationContext context_{};

public:
    template<class Source>
    LinkerEventSourceScope(
        std::type_identity<Source>,
        std::source_location location,
        std::string_view name)
    {
        LinkerEventTraceFrame frame{
            .kind = LinkerEventTraceFrame::Kind::source,
            .name = std::string(linker_event_type_name<Source>()),
            .detail = std::string(name),
            .location = location,
            .identity = static_cast<void const*>(&linker_event_module_identity<Source>),
        };
        if (auto const* active = current_linker_event_propagation) {
            throw std::logic_error(
                "event " + format_linker_event_frame(frame)
                + " was entered inside the propagation of event "
                + format_linker_event_frame(active->source));
        }
        context_.serial = next_linker_event_propagation_serial.fetch_add(
            1, std::memory_order_relaxed);
        context_.source = frame;
        context_.path.push_back(frame);
        // The source module is part of its own tree: nothing raised inside
        // the source may enter it again.
        context_.visits.emplace(frame.identity, LinkerEventModuleVisit{
            .module_name = frame.name,
            .path = context_.path,
        });
        current_linker_event_propagation = &context_;
    }

    ~LinkerEventSourceScope() { current_linker_event_propagation = nullptr; }

    LinkerEventSourceScope(LinkerEventSourceScope const&) = delete;
    LinkerEventSourceScope& operator=(LinkerEventSourceScope const&) = delete;
};

template<class Source>
LinkerEventSourceScope enter_linker_event_source(
    std::source_location location,
    std::string_view name = {})
{
    return LinkerEventSourceScope(std::type_identity<Source>{}, location, name);
}

template<class Dispatch>
decltype(auto) invoke_linker_event(
    std::string_view event_name,
    std::source_location location,
    Dispatch&& dispatch)
{
    auto* context = current_linker_event_propagation;
    if (context == nullptr) {
        std::ostringstream message;
        message << "linker event " << event_name << " was raised outside an event source";
        if (location.line() != 0) {
            message << " (" << location.file_name() << ':' << location.line() << ')';
        }
        throw std::logic_error(message.str());
    }
    LinkerEventFrameScope frame(*context, event_name, location);
    return std::forward<Dispatch>(dispatch)();
}

template<class Module, class Dispatch>
decltype(auto) invoke_linker_event_module(Dispatch&& dispatch)
{
    LinkerEventModuleFrameScope frame(std::type_identity<Module>{});
    return std::forward<Dispatch>(dispatch)();
}
} // namespace iv::details

namespace iv {
// One module's entry into the running event propagation. A module transaction
// keeps it so that later operations of the same transaction run as part of
// this one entry instead of entering the module again. Whatever those
// operations raise is still checked, as raised by this module.
class EventModuleVisit {
    std::uint64_t propagation_ = 0;
    void const* module_ = nullptr;
    std::string_view module_name_{};

    friend class EventModuleVisitScope;

public:
    EventModuleVisit() = default;

    // The entry of Module that is running now. Throws unless an event handler
    // of Module is the innermost frame of the running propagation.
    template<class Module>
    [[nodiscard]] static EventModuleVisit current()
    {
        auto const* context = details::current_linker_event_propagation;
        auto const* identity = static_cast<void const*>(
            &details::linker_event_module_identity<Module>);
        if (context == nullptr || context->path.empty()
            || context->path.back().kind != details::LinkerEventTraceFrame::Kind::module
            || context->path.back().identity != identity) {
            throw std::logic_error(
                std::string(details::linker_event_type_name<Module>())
                + " can only keep its event entry while one of its handlers runs");
        }
        EventModuleVisit visit;
        visit.propagation_ = context->serial;
        visit.module_ = identity;
        visit.module_name_ = details::linker_event_type_name<Module>();
        return visit;
    }

    [[nodiscard]] bool valid() const noexcept { return module_ != nullptr; }
};

// Runs one later operation of a module transaction inside the module's entry.
class EventModuleVisitScope {
    details::LinkerEventPropagationContext* context_ = nullptr;

public:
    explicit EventModuleVisitScope(EventModuleVisit const& visit)
        : context_(details::current_linker_event_propagation)
    {
        if (!visit.valid() || context_ == nullptr
            || context_->serial != visit.propagation_) {
            throw std::logic_error(
                "a " + std::string(visit.module_name_)
                + " transaction was used outside the event propagation that opened it");
        }
        context_->path.push_back(details::LinkerEventTraceFrame{
            .kind = details::LinkerEventTraceFrame::Kind::module,
            .name = std::string(visit.module_name_),
            .detail = "transaction",
            .identity = visit.module_,
        });
    }

    ~EventModuleVisitScope() { context_->path.pop_back(); }

    EventModuleVisitScope(EventModuleVisitScope const&) = delete;
    EventModuleVisitScope& operator=(EventModuleVisitScope const&) = delete;
};
} // namespace iv

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

// Enters an event source for the statement that follows, normally a block:
//
//     IV_EVENT_SOURCE(SocketRpcServer, request.method) { ... }
//
// The first argument is the source module; the optional second argument is a
// debugging name that collision reports print next to it. Everything raised
// inside the block is one propagation tree, in which no module, the source
// module included, may be entered twice. Linker events cannot be raised
// outside an event source, and event sources cannot nest.
#define IV_EVENT_SOURCE(source_module, ...) \
    if ([[maybe_unused]] auto iv_event_source_scope = \
            ::iv::details::enter_linker_event_source<source_module>( \
                IV_LINKER_EVENT_LOCATION __VA_OPT__(,) __VA_ARGS__); \
        true)

#define IV_INVOKE_LINKER_EVENT(event_name, ...) \
    do { \
        auto iv_linker_event_invoke = [&](auto&&... iv_linker_event_args) { \
            ::iv::details::invoke_linker_event( \
                #event_name, \
                IV_LINKER_EVENT_LOCATION, \
                [&] { \
                    for (auto const subscriber : event_name##_subscribers()) { \
                        subscriber(iv_linker_event_args...); \
                    } \
                }); \
        }; \
        iv_linker_event_invoke(__VA_ARGS__); \
    } while (false)

#define IV_INVOKE_SINGLETON_EVENT(event_name, ...) \
    ([&]() -> decltype(auto) { \
        auto iv_linker_event_invoke = [&](auto&&... iv_linker_event_args) \
            -> decltype(auto) { \
            return ::iv::details::invoke_linker_event( \
                #event_name, \
                IV_LINKER_EVENT_LOCATION, \
                [&]() -> decltype(auto) { \
                    return event_name##_subscriber()(iv_linker_event_args...); \
                }); \
        }; \
        return iv_linker_event_invoke(__VA_ARGS__); \
    }())
