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
#include <unistd.h>
#include <utility>

#if defined(IV_CLIENT_HELPER_X11)
#include <X11/Xlib.h>
#endif

namespace {
bool parse_int(std::string_view text, int& result)
{
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    return error == std::errc{} && end == text.data() + text.size();
}

#if defined(IV_CLIENT_HELPER_X11)
using Json = nlohmann::json;

void warp_cursor(Display* display, Json const& message, bool& reported_missing_window, bool& reported_origin_mismatch)
{
    if (!message.is_object() || message.contains("id") || message.find("method") == message.end()
        || message.at("method") != "native.warpCursor") return;
    auto const params = message.find("params");
    if (params == message.end() || !params->is_object()) return;
    for (auto const* field : {"originX", "originY", "deltaX", "deltaY", "expiresAt"}) {
        auto const value = params->find(field);
        if (value == params->end() || !value->is_number_integer()) return;
    }
    const auto now = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count(); };
    const auto expires_at = params->at("expiresAt").get<std::int64_t>();
    const auto received_at = now();

    const auto origin_x = params->at("originX").get<std::int64_t>();
    const auto origin_y = params->at("originY").get<std::int64_t>();
    const auto delta_x = params->at("deltaX").get<std::int64_t>();
    const auto delta_y = params->at("deltaY").get<std::int64_t>();
    const auto trace_enabled = params->contains("trace") && params->at("trace") == true;
    int root_x = 0, root_y = 0, window_x = 0, window_y = 0;
    const auto trace = [&](const char* result) {
        if (trace_enabled) std::fprintf(stderr,
            "Automation cursor trace. %s Expected (%lld, %lld), observed (%d, %d), delta (%lld, %lld).\n",
            result, static_cast<long long>(origin_x), static_cast<long long>(origin_y), root_x, root_y,
            static_cast<long long>(delta_x), static_cast<long long>(delta_y));
    };
    const auto report_x11_failure = [&](bool& reported, const char* reason) {
        trace(reason);
        if (std::exchange(reported, true)) return;
        const auto* session = std::getenv("XDG_SESSION_TYPE");
        const bool wayland = session && std::string_view(session) == "wayland";
        std::fprintf(stderr, "Automation cursor return skipped. %s%s\n", reason,
            wayland ? " On a Wayland session, cursor return requires VSCodium running through XWayland (--ozone-platform=x11)." : "");
    };
    if (expires_at <= received_at || expires_at > received_at + 100) return trace("Expired request.");
    const auto valid_coordinate = [](std::int64_t value) {
        return value >= std::numeric_limits<std::int16_t>::min() && value <= std::numeric_limits<std::int16_t>::max();
    };
    if (!valid_coordinate(origin_x) || !valid_coordinate(origin_y)
        || !valid_coordinate(delta_x) || !valid_coordinate(delta_y)
        || (delta_x == 0 && delta_y == 0)) return trace("Invalid coordinates or zero motion.");

    const Window root = DefaultRootWindow(display);
    Window pointer_root = None, under_pointer = None;
    unsigned mask = 0;
    if (!XQueryPointer(display, root, &pointer_root, &under_pointer, &root_x, &root_y, &window_x, &window_y, &mask)
        || under_pointer == None) return report_x11_failure(reported_missing_window, "No X11 window under the cursor.");
    if (root_x != origin_x || root_y != origin_y)
        return report_x11_failure(reported_origin_mismatch, "The restored X11 cursor does not match the browser origin.");

    const auto target_x = origin_x + delta_x, target_y = origin_y + delta_y;
    if (!valid_coordinate(target_x) || !valid_coordinate(target_y)) return trace("Target coordinates exceed the X11 range.");
    XWindowAttributes attributes{};
    Window unused = None;
    if (!XGetWindowAttributes(display, under_pointer, &attributes)
        || !XTranslateCoordinates(display, root, under_pointer, static_cast<int>(target_x),
            static_cast<int>(target_y), &window_x, &window_y, &unused)
        || window_x < 0 || window_y < 0 || window_x >= attributes.width || window_y >= attributes.height)
        return trace("Target is outside the X11 window where the cursor started.");
    if (expires_at <= now()) return trace("Request expired while locating the target window.");

    // Restrict the source rectangle to the lock origin. If the mouse has
    // moved since XQueryPointer, the X server ignores the warp.
    XWarpPointer(display, root, root, root_x, root_y, 1, 1, static_cast<int>(target_x), static_cast<int>(target_y));
    XFlush(display);
    if (trace_enabled) {
        XQueryPointer(display, root, &pointer_root, &under_pointer, &root_x, &root_y, &window_x, &window_y, &mask);
        trace(root_x == target_x && root_y == target_y ? "X11 cursor reached the target." : "X11 cursor did not reach the target.");
    }
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
    const char* unavailable_reason = "The native helper could not open the X11 display.";
    bool reported_missing_window = false, reported_origin_mismatch = false;
#else
    const auto cursor_warp = false;
    const char* unavailable_reason = "The native helper was built without X11 support.";
#endif
    nlohmann::json params{{"cursorWarp", cursor_warp}};
    if (!cursor_warp) params["cursorWarpUnavailableReason"] = unavailable_reason;
    const auto ready = nlohmann::json{{"jsonrpc", "2.0"}, {"method", "native.ready"}, {"params", std::move(params)}}.dump();
    if (dprintf(fd, "%s\n", ready.c_str()) < 0) return 0;

    char* line = nullptr;
    std::size_t capacity = 0;
    while (getline(&line, &capacity, input.get()) >= 0) {
#if defined(IV_CLIENT_HELPER_X11)
        if (display) warp_cursor(display.get(), Json::parse(line, nullptr, false), reported_missing_window, reported_origin_mismatch);
#endif
    }
    std::free(line);
    return std::ferror(input.get()) ? 1 : 0;
}
