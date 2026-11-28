import Clutter from 'gi://Clutter';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';

const modifier = Clutter.ModifierType;
const buttonMasks = [modifier.BUTTON1_MASK, modifier.BUTTON3_MASK, modifier.BUTTON2_MASK,
    modifier.BUTTON4_MASK, modifier.BUTTON5_MASK];

export function buttonHeld(button, state) {
    return !!(state & buttonMasks[button]);
}

function modifiers(state) {
    return {shiftKey: !!(state & modifier.SHIFT_MASK), altKey: !!(state & modifier.MOD1_MASK),
        ctrlKey: !!(state & modifier.CONTROL_MASK), metaKey: !!(state & (modifier.SUPER_MASK | modifier.META_MASK))};
}

export class PointerCapture {
    constructor(backend, relativeMotion, deliver, ended) {
        this.backend = backend;
        this.relativeMotion = relativeMotion;
        this.deliver = deliver;
        this.ended = ended;
        this.actor = new Clutter.Actor({reactive: true});
        global.stage.add_child(this.actor);
        this.seat = global.stage.get_context().get_backend().get_default_seat();
        this.tracker = backend.get_cursor_tracker();
        this.filter = Clutter.Event.add_filter(null, event => this.onEvent(event));
    }

    start(gesture, button, press, geometry) {
        if (this.active || global.stage.get_grab_actor()) return false;
        const [x, y, state] = global.get_pointer();
        if (!buttonHeld(button, state) || x < 0 || y < 0 || x > 32767 || y > 32767) return false;
        const {buffer, surfaceScale} = geometry;
        const factorX = press.windowSizeDip.width / buffer.width * surfaceScale / press.devicePixelRatio;
        const factorY = press.windowSizeDip.height / buffer.height * surfaceScale / press.devicePixelRatio;
        const originX = Math.round(buffer.x + press.windowPointDip.x * buffer.width / press.windowSizeDip.width);
        const originY = Math.round(buffer.y + press.windowPointDip.y * buffer.height / press.windowSizeDip.height);
        const capture = {gesture, button, factorX, factorY, barriers: [], samples: [], flush: 0, watch: 0};
        this.active = capture;
        try {
            // Opposing sticky barriers on each line hold the cursor even when
            // successive relative motions approach the same point from different sides.
            const directions = Meta.BarrierDirection;
            for (const [x1, y1, x2, y2, direction] of [
                [x, 0, x, 32767, directions.POSITIVE_X], [x, 0, x, 32767, directions.NEGATIVE_X],
                [0, y, 32767, y, directions.POSITIVE_Y], [0, y, 32767, y, directions.NEGATIVE_Y],
            ]) {
                capture.barriers.push(new Meta.Barrier({backend: this.backend, x1, y1, x2, y2,
                    directions: direction, flags: Meta.BarrierFlags.STICKY}));
            }
            capture.grab = global.stage.grab(this.actor);
            this.seat.inhibit_unfocus();
            capture.seatInhibited = true;
            this.tracker.inhibit_cursor_visibility();
            capture.hidden = true;
            capture.watch = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 250, () => {
                const [, , held] = global.get_pointer();
                if (global.stage.get_grab_actor() === this.actor && buttonHeld(button, held))
                    return GLib.SOURCE_CONTINUE;
                capture.watch = 0;
                this.finish({kind: 'cancel', reason: 'Mutter ended the drag.'});
                return GLib.SOURCE_REMOVE;
            });
        } catch (error) {
            this.finish({kind: 'denied', reason: 'Mutter could not grab the pointer.'});
            return false;
        }
        const startX = (x - originX) * factorX, startY = (y - originY) * factorY;
        const samples = startX || startY ? [{kind: 'motion', x: startX, y: startY, modifiers: modifiers(state)}] : [];
        this.deliver(gesture, {granted: true, samples});
        return true;
    }

    onEvent(event) {
        const capture = this.active;
        if (!capture || global.stage.get_grab_actor() !== this.actor) return Clutter.EVENT_PROPAGATE;
        const state = modifiers(event.get_state());
        switch (event.type()) {
        case Clutter.EventType.MOTION: {
            const [relative, x, y] = this.relativeMotion(event);
            if (relative && (x || y)) this.push({kind: 'motion', x: x * capture.factorX, y: y * capture.factorY, modifiers: state});
            break;
        }
        case Clutter.EventType.BUTTON_RELEASE:
            if (event.get_button() === capture.button + 1)
                this.finish({kind: 'release', modifiers: state});
            break;
        case Clutter.EventType.KEY_PRESS:
            if (event.get_key_symbol() === Clutter.KEY_Escape) {
                this.finish({kind: 'cancel', reason: 'Escape canceled the drag.'});
                return Clutter.EVENT_STOP;
            }
            this.push({kind: 'modifiers', modifiers: state});
            break;
        case Clutter.EventType.KEY_RELEASE:
            this.push({kind: 'modifiers', modifiers: state});
            break;
        case Clutter.EventType.SCROLL: {
            const direction = event.get_scroll_direction();
            const scroll = Clutter.ScrollDirection;
            const [x, y] = direction === scroll.SMOOTH ? event.get_scroll_delta() : [0, 0];
            const delta = direction === scroll.SMOOTH ? (y || x) * 100
                : direction === scroll.UP || direction === scroll.LEFT ? -100 : 100;
            if (delta) this.push({kind: 'wheel', delta, modifiers: state});
            break;
        }
        }
        return Clutter.EVENT_PROPAGATE;
    }

    push(sample) {
        const capture = this.active;
        capture.samples.push(sample);
        if (capture.samples.length >= 64) this.flush(capture);
        else if (!capture.flush) capture.flush = GLib.timeout_add_once(GLib.PRIORITY_DEFAULT, 8, () => {
            capture.flush = 0;
            this.flush(capture);
        });
    }

    flush(capture) {
        if (capture.flush) GLib.source_remove(capture.flush);
        capture.flush = 0;
        this.deliver(capture.gesture, {samples: capture.samples.splice(0)});
    }

    finish(end) {
        const capture = this.active;
        if (!capture) return;
        this.active = null;
        if (capture.flush) GLib.source_remove(capture.flush);
        if (capture.watch) GLib.source_remove(capture.watch);
        for (const barrier of capture.barriers) barrier.destroy();
        if (capture.hidden) this.tracker.uninhibit_cursor_visibility();
        if (capture.seatInhibited) this.seat.uninhibit_unfocus();
        capture.grab?.dismiss();
        this.ended(capture.gesture, end);
        this.deliver(capture.gesture, {samples: capture.samples, end});
    }

    dispose() {
        this.finish({kind: 'cancel', reason: 'The GNOME companion was disabled.'});
        Clutter.Event.remove_filter(this.filter);
        this.actor.destroy();
    }
}
