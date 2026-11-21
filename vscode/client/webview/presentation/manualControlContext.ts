import { computed, inject, provide, toValue, watch, type InjectionKey, type MaybeRefOrGetter, type Ref } from "vue";
import type { ManualControlGestures } from "../../src/manualControlGesture";
import { canEditManualControl, manualControlKey, type ManualControlId, type ManualControlDescriptor } from "../../src/manualControls";
import { knobPortStatus, knobScrubSpeed } from "../../src/knobPresentation";
import type { KnobDrag } from "../graph/context";
import { useNode } from "./nodeContext";

type Context = { gestures: ManualControlGestures; version: Ref<number>;
    setEnabled: (control: ManualControlId, enabled: boolean) => void };
const key: InjectionKey<Context> = Symbol("iv.manualControls");
export function provideManualControls(context: Context): void { provide(key, context); }
export function useOptionalManualControls(): Context | null { return inject(key, null); }
export function readManualControlValue(context: Context | null, id: ManualControlId | null,
    descriptor?: ManualControlDescriptor): number | undefined {
    void context?.version.value;
    return context && id ? context.gestures.currentValue(id, descriptor) : descriptor?.value;
}

// Semantic presentation capability; transport and backend interaction IDs stay
// in the host. Specialized components can use the same input as the fallback.
export function useManualInput(inputName: MaybeRefOrGetter<string>, direction: MaybeRefOrGetter<"input" | "output"> = "input") {
    const context = inject(key, null);
    const node = useNode();
    const id = computed<ManualControlId | null>(() => node.value.address
        ? toValue(direction) === "output" ? { node: node.value.address, outputName: toValue(inputName) }
            : { node: node.value.address, inputName: toValue(inputName) } : null);
    const port = computed(() => node.value.ports.find((entry) => entry.direction === toValue(direction)
        && entry.kind === "continuous" && entry.name === toValue(inputName)));
    const descriptor = computed(() => port.value?.kind === "continuous" ? port.value.manualControl : undefined);
    const state = computed(() => { void context?.version.value; return id.value ? context?.gestures.state(id.value) : undefined; });
    const activity = computed(() => {
        void context?.version.value;
        return id.value ? context?.gestures.activity(id.value) ?? "idle" : "idle";
    });
    const dragging = computed(() => {
        void context?.version.value;
        const pointerControl = context?.gestures.pointerControl();
        return !!id.value && !!pointerControl && manualControlKey(id.value) === manualControlKey(pointerControl);
    });
    const signedValue = computed(() => readManualControlValue(context, id.value, descriptor.value));
    const value = computed(() => toValue(direction) === "output" && signedValue.value !== undefined
        ? Math.abs(signedValue.value) : signedValue.value);
    const inverted = computed(() => {
        void context?.version.value;
        return !!id.value && signedValue.value !== undefined
            && (context?.gestures.outputInverted(id.value, signedValue.value) ?? signedValue.value < 0);
    });
    const limits = computed(() => {
        if (toValue(direction) === "output") return { defaultValue: 1, min: 0, max: undefined, scrubSpeed: undefined };
        const metadata = port.value?.kind === "continuous" ? port.value.valueTypes[0] : undefined;
        const finite = (value: unknown): number | undefined => typeof value === "number" && Number.isFinite(value) ? value : undefined;
        return { min: finite(metadata?.minValue), max: finite(metadata?.maxValue),
            defaultValue: metadata?.defaultValue === undefined ? 0 : finite(metadata?.defaultValue),
            scrubSpeed: finite(metadata?.scrubSpeed) };
    });
    const available = computed(() => !!context && !!id.value && canEditManualControl(id.value, descriptor.value)
        && typeof value.value === "number" && Number.isFinite(value.value));
    watch([descriptor, limits], () => {
        if (context && id.value && descriptor.value) context.gestures.refresh(id.value, descriptor.value, limits.value);
    });
    return {
        value, signedValue, available, limits, state, descriptor, activity, dragging, inverted, port,
        key: computed(() => id.value ? manualControlKey(id.value) : ""),
        canToggleEnabled: computed(() => id.value?.inputName !== undefined),
        hover(hovered: boolean) {
            if (!context || !id.value) return;
            if (hovered && available.value && descriptor.value)
                context.gestures.hover(id.value, descriptor.value, limits.value, knobPortStatus(port.value));
            else context.gestures.unhover(id.value);
        },
        applyValue(value: number, signed = false) {
            if (!context || !id.value || !descriptor.value) return;
            context.gestures.applyValue(id.value, descriptor.value, value, !signed);
        },
        invert() {
            if (!context || !id.value || !descriptor.value || !available.value) return;
            context.gestures.hover(id.value, descriptor.value, limits.value);
            context.gestures.invertOutput(id.value, descriptor.value);
        },
        reset() {
            if (!context || !id.value || !descriptor.value) return;
            context.gestures.hover(id.value, descriptor.value, limits.value, knobPortStatus(port.value));
            context.gestures.resetHovered();
        },
        toggleEnabled() {
            if (id.value?.inputName !== undefined && descriptor.value?.available && !descriptor.value.live && !dragging.value)
                context?.setEnabled(id.value, !descriptor.value.enabled);
        },
        numeric(anchor: DOMRect) {
            if (!id.value || !descriptor.value || !context || !available.value) return;
            context.gestures.openEntry(id.value, descriptor.value, limits.value, anchor);
        },
        scrub(event: PointerEvent): KnobDrag | undefined {
            const member = id.value, current = descriptor.value;
            if (!available.value || !member || !current || !context) return;
            const gestures = context.gestures, pointer = event.pointerId, key = manualControlKey(member);
            gestures.hover(member, current, limits.value);
            if (!gestures.scrub(member, current, pointer, -event.clientY, knobScrubSpeed(limits.value), limits.value, event.ctrlKey)) return;
            return {
                move: (point) => gestures.move(pointer, -point.y), moveBy: (delta) => gestures.moveBy(pointer, -delta.y),
                changeSensitivity: (delta) => gestures.changeSensitivity(delta),
                active: () => gestures.pointerId() === pointer,
                finish(point) {
                    const element = point && document.elementFromPoint(point.x, point.y)?.closest<HTMLElement>(".manual-input");
                    gestures.release(pointer);
                    if (element?.dataset.manualControl !== key) gestures.unhover(member);
                },
            };
        },
    };
}
