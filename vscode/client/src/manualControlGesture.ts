import type { ManualControlAction, ManualControlDescriptor, ManualControlId, ManualControlInteractionState, ManualControlLiveValue } from "./manualControls";
import { manualControlKey, canEditManualControl, manualControlName } from "./manualControls";
import { addressKey, type NodeAddress } from "./graphScope";
import { rightClickHint, type StatusHint } from "./statusBarText";
import { scrubSensitivityShortcut } from "./scrubSensitivity";
import { clampKnobValue as clamp, KnobScrub, type KnobLimits } from "./knobScrub";
import { KnobValueEditing, type KnobValueEntry } from "./knobValueEntry";

export type ManualControlLimits = KnobLimits;
export type ManualControlHint = StatusHint & { text: string };
export type KnobActivity = "idle" | "hover" | "active" | "retained";
type Hover = { control: ManualControlId; descriptor: ManualControlDescriptor; limits: ManualControlLimits; portStatus?: string };
export type ManualValueEntry = Hover & KnobValueEntry;

type Gesture = {
    interactionId: string; control: ManualControlId; value: number;
    pointer?: { id: number; scrub: KnobScrub };
};
const displayValue = (value: number) => Number(value.toPrecision(6)).toString();
export function manualKnobStatus(name: string, value: number, canReset: boolean, port = ""): ManualControlHint {
    return { text: `${port ? `${port} · ` : ""}${name} ${displayValue(value)}`,
        shortcuts: `↑↓ · ${rightClickHint} value` + (canReset ? " · Dbl-click reset" : "") };
}
// Owned by the webview root, never by a presentation widget. Components can
// disappear while these window-level pointer and keyboard gestures continue.
export class ManualControlGestures {
    private active: Gesture | null = null;
    private hovered: ManualControlId | null = null;
    private hoverDetails: Hover | null = null;
    private retainedInteraction: string | null = null;
    readonly valueEditing: KnobValueEditing<Hover>;
    private remote: ManualControlInteractionState[] = [];
    private graphGeneration: number | null = null;
    private live: ManualControlLiveValue[] = [];
    private readonly ending = new Map<string, ManualControlInteractionState>();
    // Zero carries no polarity over JSON. Keep the editor's last choice so
    // reducing an inverted gain to zero and raising it again stays inverted.
    private readonly outputInversions = new Map<string, boolean>();
    constructor(private readonly send: (action: ManualControlAction) => void,
        private readonly newId: () => string, private readonly changed: () => void,
        private readonly outputPolarity: { initial?: Record<string, boolean>;
            save?: (inverted: Record<string, boolean>) => void } = {}) {
        this.valueEditing = new KnobValueEditing(newId, changed);
        for (const [key, inverted] of Object.entries(outputPolarity.initial ?? {}))
            if (inverted === true) this.outputInversions.set(key, true);
    }
    state(control: ManualControlId): ManualControlInteractionState | undefined {
        if (this.active && manualControlKey(this.active.control) === manualControlKey(control))
            return { ...this.active, phase: "active" };
        return this.ending.get(manualControlKey(control))
            ?? this.remote.find((entry) => manualControlKey(entry.control) === manualControlKey(control)
                && (entry.phase !== "failed" || entry.graphGeneration == null || this.graphGeneration == null
                    || entry.graphGeneration === this.graphGeneration));
    }
    // The value of a gesture held elsewhere: another view or another client.
    // This view's own gesture state takes precedence through state().
    liveValue(control: ManualControlId): number | undefined {
        return this.live.find((entry) => manualControlKey(entry.control) === manualControlKey(control))?.value;
    }
    activity(control: ManualControlId): KnobActivity {
        const key = manualControlKey(control);
        if (this.active && manualControlKey(this.active.control) === key) return "active";
        const hovered = this.hovered && manualControlKey(this.hovered) === key;
        if (hovered && this.retainedInteraction) return "retained";
        if (this.liveValue(control) !== undefined) return "active";
        return hovered ? "hover" : "idle";
    }
    displayValue(control: ManualControlId, value: number): number {
        return control.outputName === undefined ? value : Math.abs(value);
    }
    outputInverted(control: ManualControlId, value: number): boolean {
        return control.outputName !== undefined && (value !== 0
            ? value < 0 : this.outputInversions.get(manualControlKey(control)) ?? false);
    }
    valueFromDisplay(control: ManualControlId, value: number): number {
        if (control.outputName === undefined) return value;
        const magnitude = Math.max(0, value);
        if (magnitude === 0) return 0;
        return this.outputInversions.get(manualControlKey(control)) ? -magnitude : magnitude;
    }
    private rememberOutputInversion(control: ManualControlId, value: number): void {
        if (control.outputName !== undefined && value !== 0)
            this.setOutputInversion(control, value < 0);
    }
    private setOutputInversion(control: ManualControlId, inverted: boolean): void {
        const key = manualControlKey(control);
        if ((this.outputInversions.get(key) ?? false) === inverted) return;
        if (inverted) this.outputInversions.set(key, true);
        else this.outputInversions.delete(key);
        this.outputPolarity.save?.(Object.fromEntries(this.outputInversions));
    }
    receive(states: ManualControlInteractionState[], live: ManualControlLiveValue[] = []): void {
        this.remote = states;
        this.live = live;
        for (const [key, pending] of this.ending) {
            if (states.some((entry) => entry.interactionId === pending.interactionId
                && (entry.phase === "settled" || entry.phase === "failed"))) this.ending.delete(key);
        }
        if (this.active && states.some((entry) => entry.interactionId === this.active?.interactionId && entry.phase === "failed"))
            this.active = null;
        if (states.some((entry) => entry.interactionId === this.retainedInteraction && entry.phase === "failed"))
            this.retainedInteraction = null;
        for (const entry of [...states, ...live]) {
            const key = manualControlKey(entry.control);
            if ((this.active && manualControlKey(this.active.control) === key) || this.ending.has(key)) continue;
            this.rememberOutputInversion(entry.control, entry.value);
        }
        this.changed();
    }
    configuredGeneration(generation: number | null): void {
        if (generation === this.graphGeneration) return;
        this.graphGeneration = generation;
        this.changed();
    }
    hover(control: ManualControlId | null, descriptor?: ManualControlDescriptor, limits: ManualControlLimits = {},
        portStatus?: string): void {
        // Pointer lock and focus changes can synthesize boundary events for
        // other knobs. They cannot transfer ownership of a held drag.
        if (this.active?.pointer && (!control
            || manualControlKey(control) !== manualControlKey(this.active.control))) return;
        if (control && descriptor) this.rememberOutputInversion(control,
            this.state(control)?.value ?? this.liveValue(control) ?? descriptor.value);
        const same = this.hovered && control && manualControlKey(this.hovered) === manualControlKey(control);
        this.hoverDetails = control && descriptor ? { control, descriptor, limits,
            portStatus: portStatus ?? (same ? this.hoverDetails?.portStatus : undefined) } : null;
        if (!same) {
            this.retainedInteraction = null;
            if (this.hovered || control) this.send({ action: "hover", control });
        }
        this.hovered = control;
        this.changed();
    }
    unhover(control: ManualControlId): void {
        // Pointer capture/lock can synthesize a leave while the physical
        // pointer remains at the knob. The root reconciles hover on release.
        if (this.active?.pointer && manualControlKey(this.active.control) === manualControlKey(control)) return;
        if (this.hovered && manualControlKey(this.hovered) === manualControlKey(control)) this.hover(null);
    }
    refresh(control: ManualControlId, descriptor: ManualControlDescriptor, limits: ManualControlLimits): void {
        const key = manualControlKey(control);
        if ((!this.active || manualControlKey(this.active.control) !== key) && !this.ending.has(key))
            this.rememberOutputInversion(control, this.state(control)?.value ?? this.liveValue(control) ?? descriptor.value);
        let changed = false;
        if (this.hoverDetails && manualControlKey(this.hoverDetails.control) === key) {
            this.hoverDetails = { ...this.hoverDetails, descriptor, limits }; changed = true;
        }
        const entry = this.valueEditing.owner();
        if (entry && manualControlKey(entry.control) === key) {
            entry.descriptor = descriptor; entry.limits = limits; changed = true;
        }
        if (this.active?.pointer && manualControlKey(this.active.control) === key) {
            this.active.pointer.scrub.refresh(limits, this.displayValue(control, this.active.value));
            changed = true;
        }
        if (changed) this.changed();
    }
    edit(control: ManualControlId, descriptor: ManualControlDescriptor): void {
        if (this.active && manualControlKey(this.active.control) === manualControlKey(control)) return;
        this.finish();
        const value = this.state(control)?.value ?? descriptor.value;
        if (!Number.isFinite(value) || !canEditManualControl(control, descriptor)) return;
        this.retainedInteraction = null;
        this.ending.delete(manualControlKey(control));
        this.rememberOutputInversion(control, value);
        this.active = { interactionId: this.newId(), control, value };
        this.send({ action: "begin", ...this.active });
        this.changed();
    }
    scrub(control: ManualControlId, descriptor: ManualControlDescriptor,
        pointerId: number, position: number, unitsPerPixel: number, limits: ManualControlLimits = {}, precise = false): boolean {
        if (this.active?.pointer) return false;
        this.cancelEntry();
        this.edit(control, descriptor);
        if (!this.active || !Number.isFinite(unitsPerPixel) || unitsPerPixel <= 0) return false;
        this.active.pointer = { id: pointerId, scrub: new KnobScrub(unitsPerPixel, position,
            this.displayValue(control, this.active.value), limits, precise) };
        this.changed();
        return true;
    }
    update(control: ManualControlId, value: number): void {
        if (!this.active || manualControlKey(this.active.control) !== manualControlKey(control) || !Number.isFinite(value)) return;
        this.active.value = value;
        this.rememberOutputInversion(control, value);
        this.send({ action: "update", interactionId: this.active.interactionId, value });
        this.changed();
    }
    move(pointerId: number, position: number, min?: number, max?: number): void {
        const pointer = this.active?.pointer;
        if (!pointer || pointer.id !== pointerId || !this.active) return;
        const { scrub } = pointer;
        const limits = min === undefined && max === undefined ? scrub.limits
            : { ...scrub.limits, min: min ?? scrub.limits.min, max: max ?? scrub.limits.max };
        const before = this.displayValue(this.active.control, this.active.value);
        const value = scrub.move(position, before, limits);
        if (value !== before) this.update(this.active.control, this.valueFromDisplay(this.active.control, value));
    }
    moveBy(pointerId: number, delta: number): void {
        const pointer = this.active?.pointer;
        if (pointer?.id === pointerId) this.move(pointerId, pointer.scrub.position + delta);
    }
    changeSensitivity(delta: number): boolean {
        const active = this.active;
        const pointer = active?.pointer;
        if (!active || !pointer) return false;
        if (pointer.scrub.changeSensitivity(delta, this.displayValue(active.control, active.value))) this.changed();
        return true;
    }

    isEditingNode(address: NodeAddress): boolean {
        return [this.active?.control, this.valueEditing.owner()?.control].some((control) =>
            control && addressKey(control.node) === addressKey(address));
    }
    openEntry(control: ManualControlId, descriptor: ManualControlDescriptor, limits: ManualControlLimits,
        anchor: ManualValueEntry["anchor"]): void {
        this.finish();
        if (!canEditManualControl(control, descriptor)) return;
        const entry = { control, descriptor, limits };
        this.valueEditing.open(manualControlName(control), this.displayValue(control, this.state(control)?.value ?? descriptor.value),
            anchor, () => entry.limits, (value) => {
                this.edit(control, entry.descriptor);
                this.update(control, this.valueFromDisplay(control, value));
                this.finish(control);
            }, entry);
    }
    numericEntry(): ManualValueEntry | null {
        const owner = this.valueEditing.owner(), view = this.valueEditing.view();
        return owner && view ? { ...owner, ...view } : null;
    }
    setEntryDraft(draft: string): void { this.valueEditing.setDraft(draft); }
    acceptEntry(): boolean { return this.valueEditing.accept(); }
    cancelEntry(): void { this.valueEditing.cancel(); }
    resetHovered(): boolean {
        if (this.active?.pointer && this.active.pointer.scrub.limits.defaultValue !== undefined) {
            this.update(this.active.control, this.valueFromDisplay(this.active.control,
                clamp(this.active.pointer.scrub.limits.defaultValue, this.active.pointer.scrub.limits)));
            this.active.pointer.scrub.resetAnchor(this.displayValue(this.active.control, this.active.value));
            return true;
        }
        const hover = this.hoverDetails;
        if (!hover || !canEditManualControl(hover.control, hover.descriptor)
            || hover.limits.defaultValue === undefined) return false;
        const value = clamp(hover.limits.defaultValue, hover.limits);
        if (this.displayValue(hover.control, this.state(hover.control)?.value ?? hover.descriptor.value) === value) return true;
        this.edit(hover.control, hover.descriptor);
        this.update(hover.control, this.valueFromDisplay(hover.control, value));
        this.finish(hover.control);
        return true;
    }
    invertOutput(control: ManualControlId, descriptor: ManualControlDescriptor): boolean {
        if (control.outputName === undefined || !canEditManualControl(control, descriptor) || this.active?.pointer) return false;
        this.cancelEntry();
        this.finish();
        const value = this.state(control)?.value ?? this.liveValue(control) ?? descriptor.value;
        if (!Number.isFinite(value)) return false;
        const inverted = !this.outputInverted(control, value);
        if (value !== 0) {
            this.edit(control, { ...descriptor, value });
            this.update(control, -value);
            this.finish(control);
        }
        this.setOutputInversion(control, inverted);
        this.changed();
        return true;
    }
    hint(): ManualControlHint | null {
        const entry = this.valueEditing.view();
        if (entry) return {
            text: entry.error ? `${entry.label} · ${entry.error}` : entry.label,
            shortcuts: entry.error ? "Esc cancel" : "Enter apply · Esc cancel",
        };
        if (this.active?.pointer) return {
            text: `${manualControlName(this.active.control)} ${displayValue(this.displayValue(this.active.control, this.active.value))}`
                + (this.outputInverted(this.active.control, this.active.value) ? " · inverted" : "")
                + ` · ${this.active.pointer.scrub.hint()}`,
            shortcuts: `${scrubSensitivityShortcut} · Esc release`,
        };
        if (!this.hoverDetails || !canEditManualControl(this.hoverDetails.control, this.hoverDetails.descriptor)) return null;
        const reset = this.hoverDetails.limits.defaultValue !== undefined;
        const { control, descriptor, portStatus } = this.hoverDetails;
        const value = this.state(control)?.value ?? this.liveValue(control) ?? descriptor.value;
        return manualKnobStatus(manualControlName(control), this.displayValue(control, value), reset, portStatus);
    }
    pointerControl(): ManualControlId | null { return this.active?.pointer ? this.active.control : null; }
    pointerId(): number | null { return this.active?.pointer?.id ?? null; }
    release(pointerId: number): void { if (this.active?.pointer?.id === pointerId) this.finish(); }
    finish(control?: ManualControlId): void {
        const active = this.active;
        if (!active || (control && manualControlKey(control) !== manualControlKey(active.control))) return;
        this.active = null;
        const key = manualControlKey(active.control);
        if (active.control.inputName !== undefined && this.hovered && manualControlKey(this.hovered) === key)
            this.retainedInteraction = active.interactionId;
        this.ending.set(key, {
            interactionId: active.interactionId, control: active.control, value: active.value, phase: "ending",
        });
        this.send({ action: "end", interactionId: active.interactionId, value: active.value });
        this.changed();
    }
    disconnected(): void {
        this.active = null; this.hovered = null; this.hoverDetails = null; this.retainedInteraction = null;
        this.valueEditing.cancel(); this.remote = []; this.live = []; this.ending.clear(); this.graphGeneration = null; this.changed();
    }
}
