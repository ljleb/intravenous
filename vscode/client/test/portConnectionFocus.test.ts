import test from "node:test";
import assert from "node:assert/strict";
import { listenPortConnectionFocus } from "../src/portConnectionFocus";
import { beginPortConnection, finishPortConnection, movePortConnection,
    type PortConnectionDrag, type PortConnectionResult } from "../src/portConnectionGesture";
import type { GraphPortEndpoint } from "../src/graphConnectionEditing";

const output: GraphPortEndpoint = { address: { instanceId: "source" }, direction: "output", kind: "sample", index: 0, name: "out" };
const input: GraphPortEndpoint = { address: { instanceId: "target" }, direction: "input", kind: "sample", index: 0, name: "in" };

test("the recorded activation blur does not cancel a drag started by the first press", () => {
    const target = new EventTarget();
    let drag: PortConnectionDrag | null = null;
    let result: PortConnectionResult | null = null;
    let documentBlurs = 0;
    const stop = listenPortConnectionFocus(target, () => { documentBlurs++; }, (focused) => {
        if (!focused) drag = null;
    });
    const pointer = (event: Event) => event as Event & { pointerId: number; button: number; buttons: number; x: number; y: number };
    target.addEventListener("pointerdown", (event) => {
        const p = pointer(event);
        drag = beginPortConnection(output, p.pointerId, p.button, p);
    });
    target.addEventListener("pointermove", (event) => {
        const p = pointer(event);
        if (drag) drag = movePortConnection(drag, p.pointerId, p.buttons, p);
    });
    target.addEventListener("pointerup", (event) => {
        const p = pointer(event);
        if (drag) result = finishPortConnection(drag, p.pointerId, p, input);
        drag = null;
    });

    // Owner's 2026-11-13 trace, starting at the activating press:
    // focus 3154.3; down 3155; accepted capture 3155.6; blur 3156.7;
    // held motion 3180; focus 3206.3; held motion 3210.5; up 4377.8.
    // Replay the observed order through production focus listeners and gesture
    // functions. This does not synthesize native window activation itself.
    const events = [
        new Event("focus"),
        Object.assign(new Event("pointerdown"), { pointerId: 1, button: 0, buttons: 1, x: 184.36, y: 77.47 }),
        new Event("blur"),
        Object.assign(new Event("pointermove"), { pointerId: 1, buttons: 1, x: 184.08, y: 77.76 }),
        new Event("focus"),
        Object.assign(new Event("pointermove"), { pointerId: 1, buttons: 1, x: 180.76, y: 79.14 }),
        Object.assign(new Event("pointerup"), { pointerId: 1, button: 0, buttons: 0, x: 87.04, y: 90.98 }),
    ];
    for (const event of events) target.dispatchEvent(event);
    assert.equal(documentBlurs, 1);
    assert.deepEqual(result, { type: "create", first: output, second: input });
    stop();
});

test("native window focus loss cancels and focus gain never revives a gesture", () => {
    const target = new EventTarget();
    let drag = beginPortConnection(output, 1, 0, { x: 10, y: 20 });
    let cancellations = 0;
    const stop = listenPortConnectionFocus(target, () => {}, (focused) => {
        if (!focused) { drag = null; cancellations++; }
    });
    const nativeFocus = (focused: boolean) => target.dispatchEvent(new MessageEvent("message", {
        data: { type: "graphWindowFocus", focused },
    }));
    nativeFocus(true);
    assert.ok(drag);
    nativeFocus(false);
    assert.equal(drag, null);
    assert.equal(cancellations, 1);
    nativeFocus(true);
    assert.equal(drag, null);
    stop();
    nativeFocus(false);
    assert.equal(cancellations, 1);
});
