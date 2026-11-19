import assert from "node:assert/strict";
import test from "node:test";
import { nextTick } from "vue";
import { useContextPopover } from "../webview/presentation/contextPopover";

function contextClick(clientX: number, clientY: number): MouseEvent {
    return Object.assign(new Event("contextmenu", { cancelable: true }), { clientX, clientY }) as MouseEvent;
}

test("only the latest context invocation opens the panel after rendering", async () => {
    const popover = useContextPopover<number>();
    const shown: Array<{ x: number; y: number }> = [];
    popover.panel.value = {
        async showAt(point) { shown.push(point); },
        close() {},
    };
    const options = { anchor: null, returnFocus: null, initialFocus: "select" };
    const first = contextClick(10, 20);
    popover.open(first, 0, options);
    assert.equal(first.defaultPrevented, true);
    popover.open(contextClick(40, 60), 0, options);
    assert.equal(popover.key.value, 0);
    await nextTick();
    assert.deepEqual(shown, [{ x: 40, y: 60 }]);
    popover.toggled(false);
    assert.equal(popover.key.value, null);
});

test("closing a context popover cancels an opening that is waiting for rendering", async () => {
    const popover = useContextPopover<string>();
    popover.panel.value = {
        async showAt() { assert.fail("A closed popover reopened"); },
        close(restoreFocus) { assert.equal(restoreFocus, true); },
    };
    popover.open(contextClick(10, 20), "max", { anchor: null, returnFocus: null, initialFocus: "button" });
    popover.close(true);
    await nextTick();
    assert.equal(popover.key.value, null);
});
