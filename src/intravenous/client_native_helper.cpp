#include <nlohmann/json.hpp>

#include <algorithm>
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
#include <X11/Xatom.h>
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

struct CursorWarpRange {
    std::int64_t begin, end;

    bool contains(std::int64_t coordinate) const { return coordinate >= begin && coordinate < end; }
};

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

Window focused_editor_window(Display* display, Window root, int editor_pid)
{
    const auto pid_property = XInternAtom(display, "_NET_WM_PID", True);
    if (pid_property == None) return None;
    Window window = None;
    int revert_to = 0;
    XGetInputFocus(display, &window, &revert_to);
    while (window != None && window != PointerRoot && window != root) {
        Atom type = None;
        int format = 0;
        unsigned long count = 0, remaining = 0;
        unsigned char* data = nullptr;
        const auto status = XGetWindowProperty(display, window, pid_property, 0, 1, False,
            XA_CARDINAL, &type, &format, &count, &remaining, &data);
        if (status != Success) return None;
        const bool has_pid = type == XA_CARDINAL && format == 32 && count == 1 && data;
        const auto pid = has_pid ? *reinterpret_cast<unsigned long const*>(data) : 0;
        if (data) XFree(data);
        if (has_pid) return pid == static_cast<unsigned long>(editor_pid) ? window : None;

        // X11 focus can be on an input child of the editor's top-level window.
        Window focus_root = None, parent = None, *children = nullptr;
        unsigned child_count = 0;
        if (!XQueryTree(display, window, &focus_root, &parent, &children, &child_count)) return None;
        if (children) XFree(children);
        if (focus_root != root) return None;
        window = parent;
    }
    return None;
}

bool warp_cursor(Display* display, bool cursor_hiding_supported, int editor_pid, Json const& message)
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
    if (hide_cursor && (mask & (Button1Mask | Button2Mask | Button3Mask))) {
        trace("A mouse button is down again.");
        return false;
    }
    const bool verify_editor_window = hide_cursor && editor_pid > 1;
    const Window source_window = verify_editor_window ? focused_editor_window(display, root, editor_pid) : under_pointer;
    if (source_window == None) return skipped("The focused X11 window does not belong to VS Code.");

    // Point returns target a specific control. A relative return after a pan
    // can follow physical movement anywhere within the verified editor window.
    const int origin_tolerance = hide_cursor ? 16 : 4;
    const auto near = [origin_tolerance](std::int64_t expected, int actual) {
        return expected >= actual - origin_tolerance && expected <= actual + origin_tolerance;
    };
    const bool scaled_origin_matches = near(origin_x, root_x) && near(origin_y, root_y);
    if (!verify_editor_window && !scaled_origin_matches && !(near(screen_x, root_x) && near(screen_y, root_y))) {
        trace("The restored X11 cursor does not match the browser origin.");
        if (!trace_enabled) {
            std::fprintf(stderr, "Cursor return skipped. The browser expected (%lld, %lld) or unscaled (%lld, %lld), but X11 reported (%d, %d).\n",
                static_cast<long long>(origin_x), static_cast<long long>(origin_y),
                static_cast<long long>(screen_x), static_cast<long long>(screen_y), root_x, root_y);
        }
        return false;
    }

    const auto sampled_x = root_x, sampled_y = root_y;
    const auto target_x = sampled_x + delta_x, target_y = sampled_y + delta_y;
    if (!valid_coordinate(target_x) || !valid_coordinate(target_y)) return skipped("Target coordinates exceed the X11 range.");
    XWindowAttributes attributes{};
    Window unused = None;
    if (!XGetWindowAttributes(display, source_window, &attributes)
        || !XTranslateCoordinates(display, root, source_window, static_cast<int>(target_x),
            static_cast<int>(target_y), &window_x, &window_y, &unused)
        || window_x < 0 || window_y < 0 || window_x >= attributes.width || window_y >= attributes.height)
        return skipped("Target is outside the X11 window where the cursor started.");

    const auto window_left = target_x - window_x, window_top = target_y - window_y;
    const auto in_window = [&](std::int64_t x, std::int64_t y) {
        return x >= window_left && x < window_left + attributes.width
            && y >= window_top && y < window_top + attributes.height;
    };
    if (verify_editor_window && !in_window(origin_x, origin_y) && !in_window(screen_x, screen_y))
        return skipped("The pan began outside the focused VS Code window.");
    const auto source_range = [verify_editor_window, origin_tolerance](
        std::int64_t origin, std::int64_t window_start, int window_size, std::int64_t delta) -> CursorWarpRange {
        // Admit only source positions whose translated destination also fits
        // inside the same window.
        const auto window_end = window_start + window_size;
        const auto begin = std::max(window_start, window_start - delta);
        const auto end = std::min(window_end, window_end - delta);
        return verify_editor_window ? CursorWarpRange{begin, end}
            : CursorWarpRange{std::max(begin, origin - origin_tolerance), std::min(end, origin + origin_tolerance + 1)};
    };
    const auto source_x = source_range(scaled_origin_matches ? origin_x : screen_x, window_left, attributes.width, delta_x);
    const auto source_y = source_range(scaled_origin_matches ? origin_y : screen_y, window_top, attributes.height, delta_y);
    const auto in_source = [&](std::int64_t x, std::int64_t y) { return source_x.contains(x) && source_y.contains(y); };
    if (hide_cursor && !in_source(root_x, root_y)) return skipped("The pointer left the cursor return window.");
    // Xorg includes the far edge of the source rectangle and treats zero
    // as the entire extent. Use the sampled target for a one-pixel band.
    const auto source_width = source_x.end - source_x.begin - 1;
    const auto source_height = source_y.end - source_y.begin - 1;
    const bool relative_warp = hide_cursor && source_width > 0 && source_height > 0;
    if (expires_at <= now()) return skipped("The request expired while locating the target window.");

    // Some XWayland servers emulate warps only while the cursor is hidden.
    // Queue hide, warp, and show on the same X connection so a renderer's
    // cursor style cannot restore visibility before the warp is processed.
    if (hide_cursor) trace(cursor_hiding_supported ? "Hiding the native cursor for the warp." : cursor_hiding_unavailable);
#if defined(IV_CLIENT_HELPER_XFIXES)
    if (hide_cursor && cursor_hiding_supported) XFixesHideCursor(display, root);
#endif
    if (relative_warp) {
        // X11 checks the source rectangle and applies the offset to the
        // pointer's current position in the same request.
        XWarpPointer(display, source_window, None, static_cast<int>(source_x.begin - window_left),
            static_cast<int>(source_y.begin - window_top), static_cast<unsigned>(source_width),
            static_cast<unsigned>(source_height), static_cast<int>(delta_x), static_cast<int>(delta_y));
    } else if (hide_cursor) {
        XWarpPointer(display, source_window, root, sampled_x - static_cast<int>(window_left),
            sampled_y - static_cast<int>(window_top), 1, 1, static_cast<int>(target_x), static_cast<int>(target_y));
    } else {
        XWarpPointer(display, root, root, root_x, root_y, 1, 1, static_cast<int>(target_x), static_cast<int>(target_y));
    }
#if defined(IV_CLIENT_HELPER_XFIXES)
    if (hide_cursor && cursor_hiding_supported) XFixesShowCursor(display, root);
#endif
    const auto at_target = [&] {
        if (root_x == target_x && root_y == target_y) return true;
        // Motion since the query changes the landing position of a relative
        // warp. Only confirm it when the new position proves it left the source.
        return relative_warp && !in_source(root_x, root_y) && in_source(root_x - delta_x, root_y - delta_y);
    };
    const bool read_back = XQueryPointer(display, root, &pointer_root, &under_pointer,
        &root_x, &root_y, &window_x, &window_y, &mask);
    if (!read_back || !at_target()) {
        if (read_back && verify_editor_window && relative_warp && (root_x != sampled_x || root_y != sampled_y)
            && in_source(root_x, root_y) && in_source(root_x - delta_x, root_y - delta_y)) {
            trace("The pointer moved during the warp. The source and destination both include its current position.");
            return false;
        }
        trace("X11 cursor did not remain at the target.");
        if (!trace_enabled) std::fprintf(stderr, "Cursor return incomplete. The target was (%lld, %lld), but X11 reported (%d, %d).\n",
            static_cast<long long>(target_x), static_cast<long long>(target_y), root_x, root_y);
        return false;
    }
    trace("X11 cursor reached the target.");
    if (trace_enabled && hide_cursor) {
        const auto landed_x = root_x, landed_y = root_y;
        std::this_thread::sleep_for(std::chrono::milliseconds{35});
        if (!XQueryPointer(display, root, &pointer_root, &under_pointer, &root_x, &root_y, &window_x, &window_y, &mask)) {
            trace("Could not read back the cursor position after 35 ms.");
            return false;
        }
        if (root_x != landed_x || root_y != landed_y) {
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
    int editor_pid = 0;
    if (const auto* value = std::getenv("VSCODE_PID"); value && !parse_int(value, editor_pid)) editor_pid = 0;
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
        const auto moved = warp_cursor(display.get(), cursor_hiding_supported, editor_pid, message);
        if (id != message.end()) {
            const auto reply = Json{{"jsonrpc", "2.0"}, {"id", *id}, {"result", {{"moved", moved}}}}.dump();
            if (dprintf(fd, "%s\n", reply.c_str()) < 0) break;
        }
#endif
    }
    std::free(line);
    return std::ferror(input.get()) ? 1 : 0;
}
