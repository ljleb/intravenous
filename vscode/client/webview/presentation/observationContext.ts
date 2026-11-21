import { computed, inject, onMounted, onScopeDispose, provide, ref, shallowRef, watch, type InjectionKey, type Ref } from "vue";
import { observationPrefetchMarginPx, observationViewportWindow, type ObservationTier } from "../../src/observationWindows";
import { useOptionalNode } from "./nodeContext";
import { observationSourceKey, type ObservationSource, type ObservationWindow, sameObservationWindow } from "../../src/observations";
import type { PresentationObservations } from "../../src/presentationObservations";

export type { PortObservation, PresentedObservation } from "../../src/presentationObservations";

const key: InjectionKey<PresentationObservations> = Symbol("iv.observations");
export function provideObservations(value: PresentationObservations): void { provide(key, value); }
export function useObservations(): PresentationObservations | null { return inject(key, null); }

const tierKey: InjectionKey<Readonly<Ref<ObservationTier | undefined>>> = Symbol("iv.observationTier");
const targets = new WeakMap<Element, (tier: ObservationTier, intersecting: boolean) => void>();
const scopeObservers = new WeakMap<Document | Element, { visible: IntersectionObserver; prefetch: IntersectionObserver }>();

export function provideObservationTier(root: Ref<HTMLElement | null>): void {
    const visible = ref(false), nearby = ref(false);
    provide(tierKey, computed(() => visible.value ? "visible" : nearby.value ? "prefetch" : undefined));
    let target: HTMLElement | null = null;
    let observers: ReturnType<typeof scopeObservers.get>;
    onMounted(() => {
        target = root.value;
        if (!target) return;
        const scrollRoot = target.closest(".timeline-scope");
        const key = scrollRoot ?? target.ownerDocument;
        const observe = (tier: ObservationTier, rootMargin?: string) => new IntersectionObserver((entries) => {
            for (const entry of entries) targets.get(entry.target)?.(tier, entry.isIntersecting && entry.intersectionRect.height > 0);
        }, { root: scrollRoot, rootMargin, threshold: [0, .0001] });
        observers = scopeObservers.get(key);
        if (!observers) scopeObservers.set(key, observers = {
            visible: observe("visible"), prefetch: observe("prefetch", `${observationPrefetchMarginPx}px 0px`),
        });
        targets.set(target, (tier, intersecting) => { (tier === "visible" ? visible : nearby).value = intersecting; });
        observers.visible.observe(target);
        observers.prefetch.observe(target);
    });
    onScopeDispose(() => {
        if (!target) return;
        targets.delete(target);
        observers?.visible.unobserve(target);
        observers?.prefetch.unobserve(target);
    });
}
export function useObservationTier() { return inject(tierKey, ref<ObservationTier>("visible")); }

export function usePresentationActive(keepActive?: () => boolean) {
    const tier = useObservationTier();
    return computed(() => tier.value === "visible" || !!keepActive?.());
}
type PortObservationViewport = {
    source: ObservationSource;
    operation: string;
    window: ObservationWindow;
    indexedEvents?: boolean;
    completeOnly?: boolean;
    consumer?: string;
};

export function usePortObservationViewport(request: () => PortObservationViewport | undefined) {
    const observations = useObservations();
    const consumer = observations ? useOptionalNode() : null;
    const tier = useObservationTier();
    const wanted = computed(request);
    const source = computed(() => {
        const value = wanted.value;
        return value && JSON.stringify([observationSourceKey(value.source), value.operation, value.window.maxDepth]);
    });
    const requested = computed<ObservationWindow | undefined>((previous) => {
        const value = wanted.value;
        const window = value && observationViewportWindow(value.window, value.indexedEvents);
        return sameObservationWindow(window, previous) ? previous : window;
    });
    const id = shallowRef<string>();
    if (observations) {
        watch([source, requested, tier], ([identity, window, active], [previous]) => {
            if (identity !== previous) {
                if (id.value) observations.observeViewport(id.value);
                id.value = identity && observations.portObservationId();
            }
            const value = wanted.value;
            const base = id.value;
            if (base) observations.observeViewport(base, value && window ? { source: value.source, operation: value.operation,
                window, consumer: value.consumer ?? consumer?.value.address } : undefined, active);
        }, { immediate: true, flush: "sync" });
        onScopeDispose(() => { if (id.value) observations.observeViewport(id.value); });
    }
    return computed(() => {
        const value = wanted.value, window = requested.value, base = id.value;
        return value && window && base && tier.value === "visible"
            ? observations?.viewportWindow(base, value.window, window, value.completeOnly ?? false) : undefined;
    });
}
