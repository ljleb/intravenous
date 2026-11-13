import { computed, inject, provide, toValue, type InjectionKey, type MaybeRefOrGetter, type Ref } from "vue";
import { ManualControlGestures } from "../../src/manualControlGesture";
import type { ManualControlId } from "../../src/manualControls";
import { useNode } from "./nodeContext";

type Context = { gestures: ManualControlGestures; version: Ref<number> };
const key: InjectionKey<Context> = Symbol("iv.manualControls");
export function provideManualControls(context: Context): void { provide(key, context); }

// Semantic presentation capability; transport and backend interaction IDs stay
// in the host. Specialized components can use the same input as the fallback.
export function useManualInput(inputName: MaybeRefOrGetter<string>) {
    const context = inject(key, null);
    const node = useNode();
    const id = computed<ManualControlId | null>(() => node.value.address
        ? { node: node.value.address, inputName: toValue(inputName) } : null);
    const port = computed(() => node.value.ports.find((entry) => entry.direction === "input"
        && entry.kind === "continuous" && entry.name === toValue(inputName)));
    const descriptor = computed(() => port.value?.kind === "continuous" ? port.value.manualControl : undefined);
    const state = computed(() => { void context?.version.value; return id.value ? context?.gestures.state(id.value) : undefined; });
    const value = computed(() => state.value?.value ?? descriptor.value?.value);
    const limits = computed(() => {
        const metadata = port.value?.kind === "continuous" ? port.value.valueTypes[0] : undefined;
        const finite = (value: unknown): number | undefined => typeof value === "number" && Number.isFinite(value) ? value : undefined;
        return { min: finite(metadata?.minValue), max: finite(metadata?.maxValue) };
    });
    const available = computed(() => !!context && !!descriptor.value?.available && !!descriptor.value.enabled
        && typeof value.value === "number" && Number.isFinite(value.value));
    return {
        value, available, limits, state, descriptor,
        hover(hovered: boolean) {
            if (!context || !id.value) return;
            if (hovered && available.value) context.gestures.hover(id.value);
            else context.gestures.unhover(id.value);
        },
        edit() { if (id.value && descriptor.value) context?.gestures.edit(id.value, descriptor.value); },
        update(value: number) {
            if (!id.value || !Number.isFinite(value)) return;
            if (limits.value.min !== undefined) value = Math.max(limits.value.min, value);
            if (limits.value.max !== undefined) value = Math.min(limits.value.max, value);
            context?.gestures.update(id.value, value);
        },
        finish() { if (id.value) context?.gestures.finish(id.value); },
        scrub(event: PointerEvent) {
            if (event.button !== 0 || !available.value || !id.value || !descriptor.value || !context) return;
            event.preventDefault(); event.stopPropagation();
            const { min, max } = limits.value;
            const scale = min !== undefined && max !== undefined && max > min
                ? (max - min) / 200 : Math.max(Math.abs(value.value ?? 0), 1) / 100;
            context.gestures.scrub(id.value, descriptor.value, event.pointerId, event.clientX, scale);
            const root = (event.currentTarget as HTMLElement).closest(".graph-editor");
            if (root instanceof HTMLElement) root.setPointerCapture(event.pointerId);
        },
    };
}
