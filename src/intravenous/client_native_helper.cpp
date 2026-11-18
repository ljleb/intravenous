#include <nlohmann/json.hpp>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>
#include <sys/prctl.h>
#include <thread>
#include <unistd.h>
#include <utility>

#if defined(IV_CLIENT_HELPER_X11)
#include <X11/Xlib.h>
#endif
#if defined(IV_CLIENT_HELPER_XFIXES)
#include <X11/extensions/Xfixes.h>
#endif

namespace {
bool parse_int(std::string_view text, int& result)
{
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    return error == std::errc{} && end == text.data() + text.size();
}

#if defined(IV_CLIENT_HELPER_X11)
using Json = nlohmann::json;

#if defined(IV_CLIENT_HELPER_XFIXES)
constexpr auto cursor_hiding_unavailable = "The X11 server does not provide XFixes 4.0 cursor hiding.";
#else
constexpr auto cursor_hiding_unavailable = "The native helper was built without XFixes cursor hiding.";
#endif

bool supports_cursor_hiding(Display* display)
{
#if defined(IV_CLIENT_HELPER_XFIXES)
    int event_base = 0, error_base = 0, major = 0, minor = 0;
    return display && XFixesQueryExtension(display, &event_base, &error_base)
        && XFixesQueryVersion(display, &major, &minor) && major >= 4;
#else
    (void)display;
    return false;
#endif
}

bool warp_cursor(Display* display, bool cursor_hiding_supported, Json const& message)
{
    auto const params = message.find("params");
    if (params == message.end() || !params->is_object()) return false;
    for (auto const* field : {"originX", "originY", "deltaX", "deltaY", "expiresAt"}) {
        auto const value = params->find(field);
        if (value == params->end() || !value->is_number_integer()) return false;
    }
    const auto now = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count(); };
    const auto expires_at = params->at("expiresAt").get<std::int64_t>();
    const auto received_at = now();

    const auto origin_x = params->at("originX").get<std::int64_t>();
    const auto origin_y = params->at("originY").get<std::int64_t>();
    auto screen_x = origin_x, screen_y = origin_y;
    if (const auto screen = params->find("screenOrigin"); screen != params->end()) {
        if (!screen->is_object()) return false;
        const auto x = screen->find("x"), y = screen->find("y");
        if (x == screen->end() || y == screen->end() || !x->is_number_integer() || !y->is_number_integer()) return false;
        screen_x = x->get<std::int64_t>();
        screen_y = y->get<std::int64_t>();
    }
    const auto delta_x = params->at("deltaX").get<std::int64_t>();
    const auto delta_y = params->at("deltaY").get<std::int64_t>();
    const auto trace_enabled = params->contains("trace") && params->at("trace") == true;
    const bool hide_cursor = params->contains("hideCursor") && params->at("hideCursor") == true;
    int root_x = 0, root_y = 0, window_x = 0, window_y = 0;
    const auto trace = [&](const char* result) {
        if (trace_enabled) std::fprintf(stderr,
            "Cursor return trace. %s Expected (%lld, %lld) or unscaled (%lld, %lld), observed (%d, %d), delta (%lld, %lld).\n",
            result, static_cast<long long>(origin_x), static_cast<long long>(origin_y),
            static_cast<long long>(screen_x), static_cast<long long>(screen_y), root_x, root_y,
            static_cast<long long>(delta_x), static_cast<long long>(delta_y));
    };
    const auto skipped = [&](const char* reason) {
        trace(reason);
        if (!trace_enabled) std::fprintf(stderr, "Cursor return skipped. %s\n", reason);
        return false;
    };
    if (expires_at <= received_at || expires_at > received_at + 100) return skipped("The request expired before the native helper received it.");
    const auto valid_coordinate = [](std::int64_t value) {
        return value >= std::numeric_limits<std::int16_t>::min() && value <= std::numeric_limits<std::int16_t>::max();
    };
    if (!valid_coordinate(delta_x) || !valid_coordinate(delta_y)
        || (delta_x == 0 && delta_y == 0)) return skipped("Invalid coordinates or zero motion.");

    const Window root = DefaultRootWindow(display);
    Window pointer_root = None, under_pointer = None;
    unsigned mask = 0;
    if (!XQueryPointer(display, root, &pointer_root, &under_pointer, &root_x, &root_y, &window_x, &window_y, &mask)
        || under_pointer == None) return skipped("No X11 window under the cursor. The editor must use X11 or XWayland.");
    // XWayland can restore the cursor a few pixels from the browser's press
    // position. XWarpPointer still checks the exact position sampled here.
    constexpr int origin_tolerance = 4;
    const auto near = [](std::int64_t expected, int actual) {
        return expected >= actual - origin_tolerance && expected <= actual + origin_tolerance;
    };
    if (!(near(origin_x, root_x) && near(origin_y, root_y)) && !(near(screen_x, root_x) && near(screen_y, root_y))) {
        trace("The restored X11 cursor does not match the browser origin.");
        if (!trace_enabled) {
            std::fprintf(stderr, "Cursor return skipped. The browser expected (%lld, %lld) or unscaled (%lld, %lld), but X11 reported (%d, %d).\n",
                static_cast<long long>(origin_x), static_cast<long long>(origin_y),
                static_cast<long long>(screen_x), static_cast<long long>(screen_y), root_x, root_y);
        }
        return false;
    }

    const auto target_x = root_x + delta_x, target_y = root_y + delta_y;
    if (!valid_coordinate(target_x) || !valid_coordinate(target_y)) return skipped("Target coordinates exceed the X11 range.");
    XWindowAttributes attributes{};
    Window unused = None;
    if (!XGetWindowAttributes(display, under_pointer, &attributes)
        || !XTranslateCoordinates(display, root, under_pointer, static_cast<int>(target_x),
            static_cast<int>(target_y), &window_x, &window_y, &unused)
        || window_x < 0 || window_y < 0 || window_x >= attributes.width || window_y >= attributes.height)
        return skipped("Target is outside the X11 window where the cursor started.");
    if (expires_at <= now()) return skipped("The request expired while locating the target window.");

    // Some XWayland servers emulate warps only while the cursor is hidden.
    // Queue hide, warp, and show on the same X connection so a renderer's
    // cursor style cannot restore visibility before the warp is processed.
    if (hide_cursor) trace(cursor_hiding_supported ? "Hiding the native cursor for the warp." : cursor_hiding_unavailable);
#if defined(IV_CLIENT_HELPER_XFIXES)
    if (hide_cursor && cursor_hiding_supported) XFixesHideCursor(display, root);
#endif
    // Restrict the source rectangle to the lock origin. If the mouse has
    // moved since XQueryPointer, the X server ignores the warp.
    XWarpPointer(display, root, root, root_x, root_y, 1, 1, static_cast<int>(target_x), static_cast<int>(target_y));
#if defined(IV_CLIENT_HELPER_XFIXES)
    if (hide_cursor && cursor_hiding_supported) XFixesShowCursor(display, root);
#endif
    if (!XQueryPointer(display, root, &pointer_root, &under_pointer, &root_x, &root_y, &window_x, &window_y, &mask)
        || root_x != target_x || root_y != target_y) {
        trace("X11 cursor did not remain at the target.");
        if (!trace_enabled) std::fprintf(stderr, "Cursor return incomplete. The target was (%lld, %lld), but X11 reported (%d, %d).\n",
            static_cast<long long>(target_x), static_cast<long long>(target_y), root_x, root_y);
        return false;
    }
    trace("X11 cursor reached the target.");
    if (trace_enabled && hide_cursor) {
        std::this_thread::sleep_for(std::chrono::milliseconds{35});
        if (!XQueryPointer(display, root, &pointer_root, &under_pointer, &root_x, &root_y, &window_x, &window_y, &mask)) {
            trace("Could not read back the cursor position after 35 ms.");
            return false;
        }
        if (root_x != target_x || root_y != target_y) {
            trace("X11 reports a different position after 35 ms.");
            return false;
        }
        trace("X11 still reports the target after 35 ms.");
    }
    // Without hiding, XWayland can update its internal position without
    // moving the visible cursor. Do not confirm a middle-button warp in that case.
    return !hide_cursor || cursor_hiding_supported;
}
#endif
}

int main(int argc, char** argv)
{
    int fd = -1, owner_pid = -1;
    if (argc != 5 || std::string_view(argv[1]) != "--rpc-fd" || std::string_view(argv[3]) != "--owner-pid"
        || !parse_int(argv[2], fd) || fd < 3 || !parse_int(argv[4], owner_pid) || owner_pid <= 1) return 2;
    // EOF owns normal shutdown. If the extension host dies during a display
    // call, the parent-death signal also terminates the helper.
    if (prctl(PR_SET_PDEATHSIG, SIGKILL, 0L, 0L, 0L) < 0 || getppid() != owner_pid) return 1;
    std::signal(SIGPIPE, SIG_IGN);
    std::unique_ptr<FILE, decltype(&std::fclose)> input(fdopen(fd, "r"), std::fclose);
    if (!input) return 1;

#if defined(IV_CLIENT_HELPER_X11)
    std::unique_ptr<Display, decltype(&XCloseDisplay)> display(XOpenDisplay(nullptr), XCloseDisplay);
    XSetErrorHandler(+[](Display*, XErrorEvent*) { return 0; });
    const auto cursor_warp = static_cast<bool>(display);
    const bool cursor_hiding_supported = supports_cursor_hiding(display.get());
    const char* unavailable_reason = "The native helper could not open the X11 display.";
#else
    const auto cursor_warp = false;
    const char* unavailable_reason = "The native helper was built without X11 support.";
#endif
    nlohmann::json params{{"cursorWarp", cursor_warp}};
    if (cursor_warp) params["cursorWarpResult"] = true;
    if (!cursor_warp) params["cursorWarpUnavailableReason"] = unavailable_reason;
    const auto ready = nlohmann::json{{"jsonrpc", "2.0"}, {"method", "native.ready"}, {"params", std::move(params)}}.dump();
    if (dprintf(fd, "%s\n", ready.c_str()) < 0) return 0;

    char* line = nullptr;
    std::size_t capacity = 0;
    while (getline(&line, &capacity, input.get()) >= 0) {
#if defined(IV_CLIENT_HELPER_X11)
        if (!display) continue;
        const auto message = Json::parse(line, nullptr, false);
        const auto method = message.find("method");
        if (method == message.end() || *method != "native.warpCursor") continue;
        const auto id = message.find("id");
        if (id != message.end() && !id->is_number_integer()) continue;
        const auto moved = warp_cursor(display.get(), cursor_hiding_supported, message);
        if (id != message.end()) {
            const auto reply = Json{{"jsonrpc", "2.0"}, {"id", *id}, {"result", {{"moved", moved}}}}.dump();
            if (dprintf(fd, "%s\n", reply.c_str()) < 0) break;
        }
#endif
    }
    std::free(line);
    return std::ferror(input.get()) ? 1 : 0;
}
