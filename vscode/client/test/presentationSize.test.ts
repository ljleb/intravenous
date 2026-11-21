import assert from "node:assert/strict";
import test from "node:test";
import { effectScope, nextTick, shallowRef } from "vue";
import { usePresentationSize } from "../webview/presentation/presentationSize";

test("presentation sizes share an observer and follow a replaced element", async () => {
    const observers: TestObserver[] = [];
    class TestObserver {
        readonly targets = new Set<Element>();
        constructor(private readonly read: (entries: Array<{ target: Element; contentRect: { width: number; height: number } }>) => void) {
            observers.push(this);
        }
        observe(target: Element) { this.targets.add(target); }
        unobserve(target: Element) { this.targets.delete(target); }
        deliver(target: Element, width: number) { this.read([{ target, contentRect: { width, height: 20 } }]); }
    }
    const previous = Object.getOwnPropertyDescriptor(globalThis, "ResizeObserver");
    Object.defineProperty(globalThis, "ResizeObserver", { configurable: true, value: TestObserver });
    const ownerDocument = {};
    const original = { ownerDocument } as Element, replacement = { ownerDocument } as Element;
    const first = effectScope(), second = effectScope();
    try {
        const element = shallowRef<Element | null>(original);
        const size = first.run(() => usePresentationSize(element))!;
        const [observer] = observers;
        observer.deliver(original, 100);
        const otherSize = second.run(() => usePresentationSize(shallowRef(original)))!;
        assert.equal(observers.length, 1);
        assert.deepEqual(size.value, { width: 100, height: 20 });
        assert.equal(otherSize.value, size.value);

        element.value = replacement;
        await nextTick();
        assert.deepEqual([...observer.targets], [original, replacement]);
        assert.equal(size.value, undefined);
        observer.deliver(replacement, 150);
        assert.equal(size.value?.width, 150);
        assert.equal(otherSize.value?.width, 100);

        second.stop();
        assert.deepEqual([...observer.targets], [replacement]);
    } finally {
        first.stop();
        second.stop();
        if (previous) Object.defineProperty(globalThis, "ResizeObserver", previous);
        else Reflect.deleteProperty(globalThis, "ResizeObserver");
    }
    assert.equal(observers[0].targets.size, 0);
});
