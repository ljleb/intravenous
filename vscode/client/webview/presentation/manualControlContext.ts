import { computed, inject, provide, toValue, watch, type InjectionKey, type MaybeRefOrGetter, type Ref } from "vue";
import { ManualControlGestures, type ManualControlLimits } from "../../src/manualControlGesture";
import { canEditManualControl, manualControlKey, type ManualControlId, type ManualControlDescriptor } from "../../src/manualControls";
import { knobPortStatus } from "../../src/knobPresentation";
import { oppositeAutomationBound, pairedAutomationBoundLimits } from "../../src/automationBoundInputs";
import { useNode } from "./nodeContext";

type Context = { gestures: ManualControlGestures; version: Ref<number>;
    setEnabled: (control: ManualControlId, enabled: boolean) => void;
    startKnob: (event: PointerEvent, control: ManualControlId, descriptor: ManualControlDescriptor, limits: ManualControlLimits) => void };
const key: InjectionKey<Context> = Symbol("iv.manualControls");
export function provideManualControls(context: Context): void { provide(key, context); }
export function useOptionalManualControls(): Context | null { return inject(key, null); }

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
    const signedValue = computed(() => {
        void context?.version.value;
        return state.value?.value ?? (id.value ? context?.gestures.liveValue(id.value) : undefined)
            ?? descriptor.value?.value;
    });
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
        const opposite = oppositeAutomationBound(node.value.definitionId, toValue(inputName));
        const paired = opposite && node.value.ports.find((entry) => entry.name === opposite && entry.direction === "input");
        const bounds = pairedAutomationBoundLimits(opposite, paired && paired.kind === "continuous" ? paired.manualControl : undefined);
        return { min: finite(metadata?.minValue), max: finite(metadata?.maxValue),
            defaultValue: metadata?.defaultValue === undefined ? 0 : finite(metadata?.defaultValue),
            scrubSpeed: finite(metadata?.scrubSpeed), ...bounds };
    });
    const available = computed(() => !!context && !!id.value && canEditManualControl(id.value, descriptor.value)
        && typeof value.value === "number" && Number.isFinite(value.value));
    watch([descriptor, limits], () => {
        if (context && id.value && descriptor.value) context.gestures.refresh(id.value, descriptor.value, limits.value);
    });
    return {
        value, available, limits, state, descriptor, activity, dragging, inverted, port,
        key: computed(() => id.value ? manualControlKey(id.value) : ""),
        hover(hovered: boolean) {
            if (!context || !id.value) return;
            if (hovered && available.value && descriptor.value)
                context.gestures.hover(id.value, descriptor.value, limits.value, knobPortStatus(port.value));
            else context.gestures.unhover(id.value);
        },
        edit() { if (id.value && descriptor.value) context?.gestures.edit(id.value, descriptor.value); },
        update(value: number) {
            if (!id.value || !Number.isFinite(value)) return;
            if (limits.value.min !== undefined) value = Math.max(limits.value.min, value);
            if (limits.value.max !== undefined) value = Math.min(limits.value.max, value);
            if (context) context.gestures.update(id.value, context.gestures.valueFromDisplay(id.value, value));
        },
        invert() {
            if (!context || !id.value || !descriptor.value || !available.value) return;
            context.gestures.hover(id.value, descriptor.value, limits.value);
            context.gestures.invertOutput(id.value, descriptor.value);
        },
        finish() { if (id.value) context?.gestures.finish(id.value); },
        toggleEnabled() {
            if (id.value?.inputName !== undefined && descriptor.value?.available && !descriptor.value.live && !dragging.value)
                context?.setEnabled(id.value, !descriptor.value.enabled);
        },
        numeric(event: MouseEvent) {
            event.preventDefault(); event.stopPropagation();
            if (!id.value || !descriptor.value || !context || !available.value) return;
            const anchor = (event.currentTarget as HTMLElement).getBoundingClientRect();
            document.querySelector<HTMLElement>(".graph-editor")?.focus({ preventScroll: true });
            context.gestures.openEntry(id.value, descriptor.value, limits.value, anchor);
        },
        scrub(event: PointerEvent) {
            if (event.button !== 0 || !available.value || !id.value || !descriptor.value || !context) return;
            event.preventDefault(); event.stopPropagation();
            context.startKnob(event, id.value, descriptor.value, limits.value);
        },
    };
}
