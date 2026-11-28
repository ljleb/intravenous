#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/prctl.h>
#include <unistd.h>
#include <utility>

#if defined(IV_CLIENT_HELPER_GIO)
#include <gio/gio.h>
#endif
#if defined(IV_CLIENT_HELPER_X11)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#endif
#if defined(IV_CLIENT_HELPER_XFIXES)
#include <X11/extensions/Xfixes.h>
#endif

namespace {
using Json = nlohmann::json;

bool parse_int(std::string_view text, int& result)
{
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
    return error == std::errc{} && end == text.data() + text.size();
}

#if defined(IV_CLIENT_HELPER_GIO) || defined(IV_CLIENT_HELPER_X11)
std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool skip_cursor_return(const char* reason)
{
    std::fprintf(stderr, "Cursor return skipped. %s\n", reason);
    return false;
}
#endif

class RpcOutput {
public:
    explicit RpcOutput(int fd) : fd_(fd) {}

    bool write(Json const& message)
    {
        const auto encoded = message.dump();
        const std::lock_guard lock(mutex_);
        return dprintf(fd_, "%s\n", encoded.c_str()) >= 0;
    }

    void notify(std::string_view method, Json params)
    {
        write({{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}});
    }

private:
    int fd_;
    std::mutex mutex_;
};

#if defined(IV_CLIENT_HELPER_GIO)
class WaylandCursor {
public:
    WaylandCursor(bool wayland, RpcOutput& output)
        : output_(output),
          bus_(wayland ? g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr) : nullptr, &g_object_unref) {}

    void start()
    {
        if (!bus_) return;
        context_ = g_main_context_new();
        loop_ = g_main_loop_new(context_, false);
        g_main_context_push_thread_default(context_);
        subscription_ = g_dbus_connection_signal_subscribe(bus_.get(), service, service, "Update", path,
            nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            +[](GDBusConnection*, const gchar* sender, const gchar*, const gchar*, const gchar*, GVariant* value, gpointer data) {
                auto& self = *static_cast<WaylandCursor*>(data);
                if (self.shell_owner_ != sender || !g_variant_is_of_type(value, G_VARIANT_TYPE("(s)"))) return;
                const char* encoded = nullptr;
                g_variant_get(value, "(&s)", &encoded);
                auto update = Json::parse(encoded, nullptr, false);
                if (update.is_object())
                    self.output_.notify("native.waylandPointerUpdate", std::move(update));
            }, this, nullptr);
        watcher_ = g_bus_watch_name_on_connection(bus_.get(), service, G_BUS_NAME_WATCHER_FLAGS_NONE,
            +[](GDBusConnection*, const gchar*, const gchar* owner, gpointer data) {
                static_cast<WaylandCursor*>(data)->verify_owner(owner);
            },
            +[](GDBusConnection*, const gchar*, gpointer data) {
                auto& self = *static_cast<WaylandCursor*>(data);
                self.shell_owner_.clear();
                self.availability(false);
            }, this, nullptr);
        g_main_context_pop_thread_default(context_);
        thread_ = g_thread_new("wayland-input", +[](gpointer loop) -> gpointer {
            g_main_loop_run(static_cast<GMainLoop*>(loop));
            return nullptr;
        }, loop_);
    }

    ~WaylandCursor()
    {
        if (!thread_) return;
        g_bus_unwatch_name(watcher_);
        g_dbus_connection_signal_unsubscribe(bus_.get(), subscription_);
        auto* stop = g_idle_source_new();
        g_source_set_callback(stop, +[](gpointer loop) -> gboolean {
            g_main_loop_quit(static_cast<GMainLoop*>(loop));
            return G_SOURCE_REMOVE;
        }, loop_, nullptr);
        g_source_attach(stop, context_);
        g_source_unref(stop);
        g_thread_join(thread_);
        g_main_loop_unref(loop_);
        g_main_context_unref(context_);
    }

    void send(std::string_view operation, int editor_pid, Json params)
    {
        if (!bus_ || editor_pid <= 1 || !params.is_object()) return;
        const auto gesture = operation == "begin" ? params.at("gesture").get<std::string>() : std::string{};
        if (operation == "begin") params["beginExpiresAt"] = now_ms() + 100;
        const auto encoded = Json{{"operation", operation}, {"editorPid", editor_pid}, {"params", std::move(params)}}.dump();
        GError* error = nullptr;
        const std::unique_ptr<GVariant, decltype(&g_variant_unref)> reply(
            g_dbus_connection_call_sync(bus_.get(), service, path, service, "Exchange",
                g_variant_new("(s)", encoded.c_str()), G_VARIANT_TYPE("(s)"),
                G_DBUS_CALL_FLAGS_NO_AUTO_START, 100, nullptr, &error), &g_variant_unref);
        const std::unique_ptr<GError, decltype(&g_error_free)> failure(error, &g_error_free);
        const char* reason = failure ? failure->message : nullptr;
        if (reply) g_variant_get(reply.get(), "(&s)", &reason);
        if (!reason || !*reason) return;
        if (operation == "begin") {
            if (!reply) send("cancel", editor_pid, {{"gesture", gesture}});
            output_.notify("native.waylandPointerUpdate", {{"gesture", gesture},
                {"end", {{"kind", "denied"}, {"reason", reason}}}});
        } else if (operation == "warp") skip_cursor_return(reason);
    }

private:
    static constexpr auto service = "org.intravenous.PointerCapture";
    static constexpr auto path = "/org/intravenous/PointerCapture";

    void verify_owner(const char* owner)
    {
        const std::unique_ptr<GVariant, decltype(&g_variant_unref)> reply(
            g_dbus_connection_call_sync(bus_.get(), "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", "org.gnome.Shell"),
                G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 100, nullptr, nullptr), &g_variant_unref);
        const char* shell = nullptr;
        if (reply) g_variant_get(reply.get(), "(&s)", &shell);
        shell_owner_ = shell && std::string_view(shell) == owner ? owner : "";
        availability(!shell_owner_.empty());
    }

    void availability(bool available)
    {
        output_.notify("native.waylandPointerAvailable", {{"available", available}});
    }

    RpcOutput& output_;
    std::unique_ptr<GDBusConnection, decltype(&g_object_unref)> bus_;
    GMainContext* context_ = nullptr;
    GMainLoop* loop_ = nullptr;
    GThread* thread_ = nullptr;
    std::string shell_owner_;
    guint subscription_ = 0, watcher_ = 0;
};
#endif

#if defined(IV_CLIENT_HELPER_X11)

struct CursorWarpRange {
    std::int64_t begin, end;

    bool contains(std::int64_t coordinate) const { return coordinate >= begin && coordinate < end; }
};

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

bool warp_cursor_to_window_point(Display* display, bool cursor_hiding_supported, int editor_pid,
    Json const& destination, std::int64_t expires_at, bool trace)
{
    const auto component = [&](const char* group, const char* key) {
        const auto object = destination.find(group);
        if (object != destination.end() && object->is_object()) {
            const auto number = object->find(key);
            if (number != object->end() && number->is_number()) return number->get<double>();
        }
        return std::numeric_limits<double>::quiet_NaN();
    };
    const auto width_dip = component("windowSizeDip", "width"), height_dip = component("windowSizeDip", "height");
    if (!(width_dip > 0 && height_dip > 0 && std::isfinite(width_dip) && std::isfinite(height_dip)))
        return skip_cursor_return("The browser window size is invalid.");

    const auto root = DefaultRootWindow(display);
    const auto window = editor_pid > 1 ? focused_editor_window(display, root, editor_pid) : None;
    XWindowAttributes attributes{};
    if (window == None || !XGetWindowAttributes(display, window, &attributes))
        return skip_cursor_return("The focused X11 window does not belong to VS Code.");

    // The window dimensions convert Chromium screen DIPs. The offset from the
    // release event to the rendered point already includes the webview zoom.
    const auto x = std::round(component("windowPointDip", "x") * attributes.width / width_dip + component("offsetPixels", "x"));
    const auto y = std::round(component("windowPointDip", "y") * attributes.height / height_dip + component("offsetPixels", "y"));
    if (!(x >= 0 && x < attributes.width && x <= std::numeric_limits<std::int16_t>::max()
            && y >= 0 && y < attributes.height && y <= std::numeric_limits<std::int16_t>::max()))
        return skip_cursor_return("The control point is outside the focused VS Code window.");
    if (expires_at <= now_ms()) return skip_cursor_return("The request expired while locating the target window.");
    if (trace) std::fprintf(stderr, "Cursor return trace. X11 window %lu, size %d x %d, destination (%.0f, %.0f).\n",
        window, attributes.width, attributes.height, x, y);

#if defined(IV_CLIENT_HELPER_XFIXES)
    if (cursor_hiding_supported) XFixesHideCursor(display, root);
#endif
    XWarpPointer(display, None, window, 0, 0, 0, 0, static_cast<int>(x), static_cast<int>(y));
#if defined(IV_CLIENT_HELPER_XFIXES)
    if (cursor_hiding_supported) XFixesShowCursor(display, root);
#else
    (void)cursor_hiding_supported;
#endif
    XSync(display, False);
    return true;
}

bool warp_cursor(Display* display, bool cursor_hiding_supported, int editor_pid, Json const& message)
{
    auto const params = message.find("params");
    if (params == message.end() || !params->is_object()) return false;
    const auto expiry = params->find("expiresAt");
    if (expiry == params->end() || !expiry->is_number_integer()) return false;
    const auto expires_at = expiry->get<std::int64_t>();
    const auto received_at = now_ms();
    if (expires_at <= received_at || expires_at > received_at + 100)
        return skip_cursor_return("The request expired before the native helper received it.");
    const bool trace = params->contains("trace") && params->at("trace") == true;
    if (auto const destination = params->find("destination"); destination != params->end())
        return warp_cursor_to_window_point(display, cursor_hiding_supported, editor_pid, *destination, expires_at, trace);

    for (auto const* field : {"originX", "originY", "deltaX", "deltaY"}) {
        auto const value = params->find(field);
        if (value == params->end() || !value->is_number_integer()) return false;
    }

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
    const bool hide_cursor = params->contains("hideCursor") && params->at("hideCursor") == true;
    const bool follow_pointer = params->contains("followPointer") && params->at("followPointer") == true;
    int root_x = 0, root_y = 0, window_x = 0, window_y = 0;
    const auto skipped = skip_cursor_return;
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
        if (trace) return skipped("A mouse button was still held at cursor return.");
        return false;
    }
    const bool verify_editor_window = follow_pointer && editor_pid > 1;
    const Window source_window = verify_editor_window ? focused_editor_window(display, root, editor_pid) : under_pointer;
    if (source_window == None) return skipped("The focused X11 window does not belong to VS Code.");

    // Point returns target a specific control. A relative return after a pan
    // can follow physical movement anywhere within the verified editor window.
    const int origin_tolerance = follow_pointer ? 16 : 4;
    const auto near = [origin_tolerance](std::int64_t expected, int actual) {
        return expected >= actual - origin_tolerance && expected <= actual + origin_tolerance;
    };
    const bool scaled_origin_matches = near(origin_x, root_x) && near(origin_y, root_y);
    if (!verify_editor_window && !scaled_origin_matches && !(near(screen_x, root_x) && near(screen_y, root_y))) {
        const auto screen = DefaultScreen(display);
        const auto root_width = DisplayWidth(display, screen);
        const auto root_height = DisplayHeight(display, screen);
        const bool at_root_center = root_x == root_width / 2 && root_y == root_height / 2;
        const char* editor_focus = editor_pid <= 1 ? "unknown (VSCODE_PID unavailable)"
            : focused_editor_window(display, root, editor_pid) != None ? "yes" : "no";
        std::fprintf(stderr, "Cursor return skipped. The browser expected (%lld, %lld) or unscaled (%lld, %lld), but X11 reported (%d, %d)%s. "
            "X11 root size: %d x %d. VS Code owns X11 focus: %s. Requested warp: %s.\n",
            static_cast<long long>(origin_x), static_cast<long long>(origin_y),
            static_cast<long long>(screen_x), static_cast<long long>(screen_y), root_x, root_y,
            at_root_center ? " at the root center" : "", root_width, root_height, editor_focus,
            follow_pointer ? "relative" : "absolute");
        return false;
    }

    const auto sampled_x = root_x, sampled_y = root_y;
    const auto matched_x = scaled_origin_matches ? origin_x : screen_x;
    const auto matched_y = scaled_origin_matches ? origin_y : screen_y;
    const auto target_x = (follow_pointer ? sampled_x : matched_x) + delta_x;
    const auto target_y = (follow_pointer ? sampled_y : matched_y) + delta_y;
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
    const auto source_x = source_range(matched_x, window_left, attributes.width, delta_x);
    const auto source_y = source_range(matched_y, window_top, attributes.height, delta_y);
    const auto in_source = [&](std::int64_t x, std::int64_t y) { return source_x.contains(x) && source_y.contains(y); };
    if (follow_pointer && !in_source(root_x, root_y)) return skipped("The pointer left the cursor return window.");
    // Xorg includes the far edge of the source rectangle and treats zero
    // as the entire extent. Use the sampled target for a one-pixel band.
    const auto source_width = source_x.end - source_x.begin - 1;
    const auto source_height = source_y.end - source_y.begin - 1;
    const bool relative_warp = follow_pointer && source_width > 0 && source_height > 0;
    if (expires_at <= now_ms()) return skipped("The request expired while locating the target window.");

    // Some XWayland servers emulate warps only while the cursor is hidden.
    // Queue hide, warp, and show on the same X connection so a renderer's
    // cursor style cannot restore visibility before the warp is processed.
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
    if (trace) {
        std::fprintf(stderr, "Cursor return trace. X11 sampled (%d, %d), target (%lld, %lld), read back (%d, %d). "
            "Read succeeded: %s. Cursor hiding requested: %s, available: %s. Relative warp: %s.\n",
            sampled_x, sampled_y, static_cast<long long>(target_x), static_cast<long long>(target_y),
            root_x, root_y, read_back ? "yes" : "no", hide_cursor ? "yes" : "no",
            cursor_hiding_supported ? "yes" : "no", relative_warp ? "yes" : "no");
    }
    if (!read_back || !at_target()) {
        if (read_back && verify_editor_window && relative_warp && (root_x != sampled_x || root_y != sampled_y)
            && in_source(root_x, root_y) && in_source(root_x - delta_x, root_y - delta_y)) return false;
        std::fprintf(stderr, "Cursor return incomplete. The target was (%lld, %lld), but X11 reported (%d, %d).\n",
            static_cast<long long>(target_x), static_cast<long long>(target_y), root_x, root_y);
        return false;
    }
    // Without hiding, XWayland can update its internal position without
    // moving the visible cursor. Confirm a hidden warp only with XFixes support.
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

    [[maybe_unused]] const auto* wayland_display = std::getenv("WAYLAND_DISPLAY");
    [[maybe_unused]] int editor_pid = 0;
    if (const auto* value = std::getenv("VSCODE_PID"); value && !parse_int(value, editor_pid)) editor_pid = 0;
    RpcOutput output(fd);
#if defined(IV_CLIENT_HELPER_GIO)
    WaylandCursor wayland_cursor(wayland_display && *wayland_display && editor_pid > 1, output);
#endif
    bool cursor_warp = false;
    const char* unavailable_reason = "The native helper was built without X11 support.";
#if defined(IV_CLIENT_HELPER_X11)
    std::unique_ptr<Display, decltype(&XCloseDisplay)> display(XOpenDisplay(nullptr), XCloseDisplay);
    XSetErrorHandler(+[](Display*, XErrorEvent*) { return 0; });
    const bool cursor_hiding_supported = supports_cursor_hiding(display.get());
    cursor_warp = static_cast<bool>(display);
    unavailable_reason = "The native helper could not open the X11 display.";
#endif
    Json params{{"cursorWarp", cursor_warp}, {"cursorWarpResult", cursor_warp}};
#if defined(IV_CLIENT_HELPER_GIO)
    params["waylandCandidate"] = wayland_display && *wayland_display && editor_pid > 1;
#endif
    if (!cursor_warp) params["cursorWarpUnavailableReason"] = unavailable_reason;
    output.notify("native.ready", std::move(params));
#if defined(IV_CLIENT_HELPER_GIO)
    wayland_cursor.start();
#endif

    char* line = nullptr;
    std::size_t capacity = 0;
    while (getline(&line, &capacity, input.get()) >= 0) {
        const auto message = Json::parse(line, nullptr, false);
        const auto method = message.find("method");
        const auto input_params = message.find("params");
        if (method == message.end() || input_params == message.end() || !input_params->is_object()) continue;
        const auto id = message.find("id");
        if (id != message.end() && !id->is_number_integer()) continue;
        bool moved = false;
        if (*method == "native.beginWaylandPointer" || *method == "native.cancelWaylandCursor") {
#if defined(IV_CLIENT_HELPER_GIO)
            if (input_params->contains("gesture") && (*input_params)["gesture"].is_string())
                wayland_cursor.send(*method == "native.beginWaylandPointer" ? "begin" : "cancel", editor_pid, *input_params);
#endif
        } else if (*method == "native.warpCursor") {
            if (input_params->contains("waylandGesture")) {
#if defined(IV_CLIENT_HELPER_GIO)
                const auto expiration = input_params->find("expiresAt");
                if (expiration != input_params->end() && expiration->is_number_integer()) {
                    const auto timeout = expiration->get<std::int64_t>();
                    const auto now = now_ms();
                    if (timeout > now && timeout <= now + 100) wayland_cursor.send("warp", editor_pid, *input_params);
                }
#endif
            } else {
#if defined(IV_CLIENT_HELPER_X11)
                if (display) moved = warp_cursor(display.get(), cursor_hiding_supported, editor_pid, message);
#endif
            }
        } else continue;
        if (id != message.end() && !output.write({{"jsonrpc", "2.0"}, {"id", *id}, {"result", {{"moved", moved}}}})) break;
    }
    std::free(line);
    return std::ferror(input.get()) ? 1 : 0;
}
