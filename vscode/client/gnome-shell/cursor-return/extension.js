import GIRepository from 'gi://GIRepository?version=3.0';
import {PointerCapture, buttonHeld} from './pointerCapture.js';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

const busName = 'org.intravenous.PointerCapture';
const objectPath = '/org/intravenous/PointerCapture';
const interfaceXml = `<node><interface name="${busName}">
<method name="Exchange"><arg type="s" direction="in"/><arg type="s" direction="out"/></method>
<signal name="Update"><arg type="s"/></signal>
</interface></node>`;
const nowMs = () => Math.floor(GLib.get_real_time() / 1000);

function clientPid(sender) {
    return new Promise((resolve, reject) => {
        const bus = Gio.DBus.session;
        bus.call('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
            'GetConnectionUnixProcessID', new GLib.Variant('(s)', [sender]),
            new GLib.VariantType('(u)'), Gio.DBusCallFlags.NO_AUTO_START, 100, null, (source, result) => {
                try {
                    resolve(source.call_finish(result).deepUnpack()[0]);
                } catch (error) {
                    reject(error);
                }
            });
    });
}

function belongsToEditor(process, editor) {
    if (!Number.isInteger(process) || !Number.isInteger(editor) || editor <= 1) return false;
    for (let depth = 0; process > 1 && depth < 32; ++depth) {
        if (process === editor) return depth > 0;
        try {
            const [, bytes] = GLib.file_get_contents(`/proc/${process}/stat`);
            const record = new TextDecoder().decode(bytes);
            process = Number(record.slice(record.lastIndexOf(')') + 2).split(' ', 2)[1]);
        } catch {
            return false;
        }
    }
    return false;
}

function contains(rect, point) {
    return point.x >= rect.x && point.x < rect.x + rect.width
        && point.y >= rect.y && point.y < rect.y + rect.height;
}

function sameGeometry(a, b) {
    return a.x === b.x && a.y === b.y && a.width === b.width && a.height === b.height;
}

function snapshot(window) {
    const frame = window.get_frame_rect();
    let surfaceScale = global.display.get_monitor_scale(window.get_monitor());
    for (let monitor = 0; monitor < global.display.get_n_monitors(); ++monitor) {
        const display = global.display.get_monitor_geometry(monitor);
        if (frame.x < display.x + display.width && display.x < frame.x + frame.width
            && frame.y < display.y + display.height && display.y < frame.y + frame.height)
            surfaceScale = Math.max(surfaceScale, global.display.get_monitor_scale(monitor));
    }
    const actor = window.get_compositor_private();
    const transitioning = !actor || actor.scale_x !== 1 || actor.scale_y !== 1
        || ['x', 'y', 'scale-x', 'scale-y', 'translation-x', 'translation-y'].some(name => actor.get_transition(name));
    return {frame, buffer: window.get_buffer_rect(), surfaceScale, transitioning};
}

function cursorPoint(destination, geometry) {
    const { windowSizeDip: size, windowPointDip: origin, offsetPixels: offset } = destination ?? {};
    if (![size?.width, size?.height, geometry.surfaceScale].every(value => Number.isFinite(value) && value > 0)
        || ![origin?.x, origin?.y, offset?.x, offset?.y].every(Number.isFinite))
        return null;
    const { buffer, surfaceScale } = geometry;
    return {
        x: Math.round(buffer.x + (origin.x + offset.x / surfaceScale) * buffer.width / size.width),
        y: Math.round(buffer.y + (origin.y + offset.y / surfaceScale) * buffer.height / size.height),
    };
}

export default class CursorReturn extends Extension {
    enable() {
        this.seat = global.stage.get_context().get_backend().get_default_seat();
        GIRepository.Repository.dup_default().prepend_search_path(this.path);
        const relativeMotion = imports.gi.IvClutter.relative_motion;
        this.gestures = new Map();
        this.capture = new PointerCapture(global.display.get_context().get_backend(), relativeMotion,
            (gesture, update) => Gio.DBus.session.emit_signal(gesture.sender, objectPath, busName, 'Update',
                new GLib.Variant('(s)', [JSON.stringify({gesture: gesture.id, ...update})])),
            (gesture, end) => {
                if (end.kind === 'release') gesture.released = true;
                else if (this.gestures.get(gesture.sender) === gesture) this.gestures.delete(gesture.sender);
            });
        this.focusSignal = global.display.connect('notify::focus-window', () => {
            this.capture.finish({kind: 'cancel', reason: 'The editor lost focus.'});
            this.gestures.clear();
        });
        const bus = Gio.DBus.session;
        this.disconnectSignal = bus.signal_subscribe('org.freedesktop.DBus', 'org.freedesktop.DBus',
            'NameOwnerChanged', '/org/freedesktop/DBus', null, Gio.DBusSignalFlags.NONE,
            (_connection, _sender, _path, _interface, _signal, parameters) => {
                const [name, , owner] = parameters.deepUnpack();
                if (owner) return;
                if (this.capture.active?.gesture.sender === name)
                    this.capture.finish({kind: 'cancel', reason: 'The editor input bridge disconnected.'});
                this.gestures.delete(name);
            });
        this.service = Gio.DBusExportedObject.wrapJSObject(interfaceXml, this);
        this.service.export(bus, objectPath);
        this.busOwner = Gio.bus_own_name_on_connection(bus, busName, Gio.BusNameOwnerFlags.NONE, null, null);
    }

    disable() {
        this.capture.dispose();
        Gio.bus_unown_name(this.busOwner);
        this.service.unexport();
        Gio.DBus.session.signal_unsubscribe(this.disconnectSignal);
        global.display.disconnect(this.focusSignal);
        this.capture = null;
        this.gestures = null;
        this.seat = null;
    }

    async ExchangeAsync([encoded], invocation) {
        let reason;
        try {
            reason = encoded.length <= 4096
                ? await this.exchange(invocation.get_sender(), JSON.parse(encoded)) : 'Request is too large.';
        } catch {
            reason = 'Could not process the Mutter cursor return request.';
        }
        invocation.return_value(new GLib.Variant('(s)', [reason]));
    }

    async exchange(sender, { operation, editorPid, params }) {
        const gestures = this.gestures;
        const gesture = operation === 'warp' ? params?.waylandGesture : params?.gesture;
        if (!gestures || typeof gesture !== 'string'
            || !/^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(gesture))
            return 'The Wayland cursor gesture is invalid.';
        const previous = gestures.get(sender);
        if (operation === 'cancel') {
            if (previous?.id === gesture) {
                if (this.capture.active?.gesture === previous)
                    this.capture.finish({kind: 'cancel', reason: 'The editor canceled the drag.'});
                gestures.delete(sender);
            }
            return '';
        }
        const window = global.display.focus_window;
        if (operation === 'begin') {
            if (!window || window.get_client_type() !== Meta.WindowClientType.WAYLAND || window.get_pid() !== editorPid)
                return 'The focused Wayland window does not belong to the editor.';
            if (this.capture.active) return 'Another gesture already owns the pointer.';
            const {windowPointDip, windowSizeDip, time, devicePixelRatio, button, beginExpiresAt} = params;
            const timely = () => {
                const remaining = beginExpiresAt - nowMs();
                return Number.isSafeInteger(beginExpiresAt) && remaining > 0 && remaining <= 100;
            };
            if (![windowSizeDip?.width, windowSizeDip?.height, devicePixelRatio].every(value => Number.isFinite(value) && value > 0)
                || ![windowPointDip?.x, windowPointDip?.y, time].every(Number.isFinite)
                || !Number.isInteger(button) || button < 0 || button > 4)
                return 'The pointer press data is invalid.';
            const userTime = window.get_user_time();
            const pressTime = GLib.get_monotonic_time() / 1000 + time - GLib.get_real_time() / 1000;
            if (!timely() || Math.abs((Math.trunc(pressTime) - userTime) | 0) > 8 || !buttonHeld(button, global.get_pointer()[2]))
                return 'The pointer press is no longer active.';
            const pending = {id: gesture, sender, editorPid, window, userTime};
            gestures.set(sender, pending);
            const pid = await clientPid(sender).catch(() => 0);
            if (this.gestures !== gestures || gestures.get(sender) !== pending)
                return 'The gesture was superseded.';
            if (global.display.focus_window !== window || window.get_user_time() !== userTime || !belongsToEditor(pid, editorPid)) {
                gestures.delete(sender);
                return 'The focused editor or its input changed.';
            }
            const geometry = snapshot(window);
            if (!timely() || geometry.transitioning
                || geometry.buffer.width <= 0 || geometry.buffer.height <= 0
                || !this.capture.start(pending, button, params, geometry)) {
                gestures.delete(sender);
                return 'Mutter could not capture the current press.';
            }
            pending.geometry = geometry;
            return '';
        }
        if (operation !== 'warp' || !previous?.released || previous.id !== gesture || this.capture.active)
            return 'The Wayland cursor gesture is no longer active.';
        gestures.delete(sender);
        if (previous.window !== window || previous.editorPid !== editorPid || window.get_user_time() !== previous.userTime)
            return 'Focus or the most recent input changed during the drag.';
        const geometry = snapshot(window);
        if (geometry.transitioning || geometry.surfaceScale !== previous.geometry.surfaceScale
            || !sameGeometry(geometry.buffer, previous.geometry.buffer))
            return 'The window geometry changed during the drag.';
        const point = cursorPoint(params.destination, geometry);
        if (!point || !contains(geometry.frame, point))
            return 'The cursor destination is outside the editor window.';
        const remaining = params.expiresAt - nowMs();
        if (!Number.isSafeInteger(params.expiresAt) || remaining <= 0 || remaining > 100)
            return 'The Wayland cursor return request expired.';
        for (let monitor = 0; monitor < global.display.get_n_monitors(); ++monitor) {
            if (contains(global.display.get_monitor_geometry(monitor), point)) {
                this.seat.warp_pointer(point.x, point.y);
                return '';
            }
        }
        return 'The cursor destination is outside the display.';
    }
}
