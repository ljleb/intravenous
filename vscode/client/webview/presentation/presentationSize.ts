import { onScopeDispose, shallowRef, watch, type Ref } from "vue";

type Size = { width: number; height: number };
const observers = new WeakMap<Document, ResizeObserver>();
const readers = new WeakMap<Element, { last?: Size; listeners: Set<(size: Size) => void> }>();

// A mounted presentation tracks its current element even when its body
// switches views. One observer delivers all size changes for the webview.
export function usePresentationSize(element: Readonly<Ref<Element | null>>) {
    const size = shallowRef<Size>();
    let stop: (() => void) | undefined;
    watch(element, (target) => {
        stop?.();
        stop = undefined;
        size.value = undefined;
        if (!target) return;
        const document = target.ownerDocument;
        let observer = observers.get(document);
        if (!observer) {
            observer = new ResizeObserver((entries) => {
                for (const { target, contentRect: { width, height } } of entries) {
                    const entry = readers.get(target);
                    if (!entry || (entry.last?.width === width && entry.last.height === height)) continue;
                    entry.last = { width, height };
                    for (const listener of entry.listeners) listener(entry.last);
                }
            });
            observers.set(document, observer);
        }
        const listener = (value: Size) => { size.value = value; };
        const existing = readers.get(target);
        const entry = existing ?? { listeners: new Set() };
        entry.listeners.add(listener);
        if (existing) size.value = existing.last;
        else {
            readers.set(target, entry);
            observer.observe(target);
        }
        stop = () => {
            entry.listeners.delete(listener);
            if (entry.listeners.size) return;
            readers.delete(target);
            observer.unobserve(target);
        };
    }, { immediate: true, flush: "post" });
    onScopeDispose(() => stop?.());
    return size;
}
