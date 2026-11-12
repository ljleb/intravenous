#pragma once

#include <intravenous/linker_event.h>

#include <source_location>
#include <string_view>
#include <utility>

namespace iv::test {
// The event source of a test. Each test stimulus that raises linker events is
// one entry of this source, as one client message is one entry of
// SocketRpcServer.
struct TestEventSource {};

// Runs `body` as one event source entry and returns its result.
template<class Body>
decltype(auto) in_event_source(
    Body&& body,
    std::string_view name = "test",
    std::source_location location = std::source_location::current())
{
    [[maybe_unused]] auto scope =
        ::iv::details::enter_linker_event_source<TestEventSource>(location, name);
    return std::forward<Body>(body)();
}
} // namespace iv::test

using iv::test::in_event_source;
