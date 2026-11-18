import type { ManualControlAction, ManualControlDescriptor, ManualControlId, ManualControlInteractionState, ManualControlLiveValue } from "./manualControls";
import { manualControlKey, manualControlName } from "./manualControls";
import { addressKey, type NodeAddress } from "./graphScope";
import { rightClickHint } from "./statusBarText";

export type ManualControlLimits = { min?: number; max?: number; defaultValue?: number; scrubSpeed?: number };
export type ManualControlHint = { text: string };
type Hover = { control: ManualControlId; descriptor: ManualControlDescriptor; limits: ManualControlLimits };
export type ManualValueEntry = Hover & {
    id: string; draft: string; error: string;
    anchor: { left: number; top: number; bottom: number };
};

type Gesture = {
    interactionId: string; control: ManualControlId; value: number;
    pointer?: { id: number; position: number; initialScale: number; speed: number;
        plateau: number; limits: ManualControlLimits;
        anchorPosition: number; anchorValue: number; anchorPlateau: number };
};
const plateauHalfWidth = 12; // Extra pixels on either side of the exact default.
const clamp = (value: number, limits: ManualControlLimits) =>
    Math.max(limits.min ?? -Infinity, Math.min(limits.max ?? Infinity, value));
const displayValue = (value: number) => Number(value.toPrecision(6)).toString();
export function manualKnobStatus(name: string, value: number, canReset: boolean): string {
    return `${name} ${displayValue(value)} · Drag/↑↓ · Ctrl ¼× · ${rightClickHint} value`
        + (canReset ? " · Dbl-click reset" : "");
}
// Owned by the webview root, never by a presentation widget. Components can
// disappear while these window-level pointer and keyboard gestures continue.
export class ManualControlGestures {
    private active: Gesture | null = null;
    private hovered: ManualControlId | null = null;
    private hoverDetails: Hover | null = null;
    private entry: ManualValueEntry | null = null;
    private remote: ManualControlInteractionState[] = [];
    private live: ManualControlLiveValue[] = [];
    private readonly ending = new Map<string, ManualControlInteractionState>();
    // Zero carries no polarity over JSON. Keep the editor's last choice so
    // reducing an inverted gain to zero and raising it again stays inverted.
    private readonly outputInversions = new Map<string, boolean>();
    constructor(private readonly send: (action: ManualControlAction) => void,
        private readonly newId: () => string, private readonly changed: () => void,
        private readonly outputPolarity: { initial?: Record<string, boolean>;
            save?: (inverted: Record<string, boolean>) => void } = {}) {
        for (const [key, inverted] of Object.entries(outputPolarity.initial ?? {}))
            if (inverted === true) this.outputInversions.set(key, true);
    }
    state(control: ManualControlId): ManualControlInteractionState | undefined {
        if (this.active && manualControlKey(this.active.control) === manualControlKey(control))
            return { ...this.active, phase: "active" };
        return this.ending.get(manualControlKey(control))
            ?? this.remote.find((entry) => manualControlKey(entry.control) === manualControlKey(control));
    }
    // The value of a gesture held elsewhere: another view or another client.
    // This view's own gesture state takes precedence through state().
    liveValue(control: ManualControlId): number | undefined {
        return this.live.find((entry) => manualControlKey(entry.control) === manualControlKey(control))?.value;
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
        for (const entry of [...states, ...live]) {
            const key = manualControlKey(entry.control);
            if ((this.active && manualControlKey(this.active.control) === key) || this.ending.has(key)) continue;
            this.rememberOutputInversion(entry.control, entry.value);
        }
        this.changed();
    }
    hover(control: ManualControlId | null, descriptor?: ManualControlDescriptor, limits: ManualControlLimits = {}): void {
        // Pointer lock and focus changes can synthesize boundary events for
        // other knobs. They cannot transfer ownership of a held drag.
        if (this.active?.pointer && (!control
            || manualControlKey(control) !== manualControlKey(this.active.control))) return;
        if (control && descriptor) this.rememberOutputInversion(control,
            this.state(control)?.value ?? this.liveValue(control) ?? descriptor.value);
        this.hoverDetails = control && descriptor ? { control, descriptor, limits } : null;
        const same = this.hovered && control && manualControlKey(this.hovered) === manualControlKey(control);
        if (!same && (this.hovered || control)) this.send({ action: "hover", control });
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
            this.hoverDetails = { control, descriptor, limits }; changed = true;
        }
        if (this.entry && manualControlKey(this.entry.control) === key) {
            this.entry.descriptor = descriptor; this.entry.limits = limits; changed = true;
        }
        if (this.active?.pointer && manualControlKey(this.active.control) === key) {
            const old = this.active.pointer.limits;
            if (old.defaultValue !== limits.defaultValue) this.active.pointer.plateau = 0;
            if (old.defaultValue !== limits.defaultValue || old.min !== limits.min || old.max !== limits.max)
                this.anchorPointer();
            this.active.pointer.limits = limits; changed = true;
        }
        if (changed) this.changed();
    }
    edit(control: ManualControlId, descriptor: ManualControlDescriptor): void {
        if (this.active && manualControlKey(this.active.control) === manualControlKey(control)) return;
        this.finish();
        const value = this.state(control)?.value ?? descriptor.value;
        if (!Number.isFinite(value) || !descriptor.available || !descriptor.enabled) return;
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
        this.active.pointer = { id: pointerId, position, initialScale: unitsPerPixel, speed: precise ? -2 : 0, plateau: 0, limits,
            anchorPosition: position, anchorValue: this.displayValue(control, this.active.value), anchorPlateau: 0 };
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
        // Recompute from one anchor for this sensitivity instead of adding
        // rounded increments. Identical travel must not depend on event count.
        const delta = position - pointer.anchorPosition;
        pointer.position = position;
        const limits = { ...pointer.limits, min: min ?? pointer.limits.min, max: max ?? pointer.limits.max };
        const scale = pointer.initialScale * 2 ** pointer.speed;
        const before = this.displayValue(this.active.control, this.active.value);
        const target = limits.defaultValue;
        let value = pointer.anchorValue + delta * scale;
        if (target !== undefined && Number.isFinite(target) && clamp(target, limits) === target) {
            // Numeric values occupy their full range. Only the exact default
            // gets extra physical travel, retained across sensitivity changes.
            const start = pointer.anchorValue < target ? (pointer.anchorValue - target) / scale - plateauHalfWidth
                : pointer.anchorValue > target ? (pointer.anchorValue - target) / scale + plateauHalfWidth : pointer.anchorPlateau;
            const end = start + delta;
            value = end < -plateauHalfWidth ? target + (end + plateauHalfWidth) * scale
                : end > plateauHalfWidth ? target + (end - plateauHalfWidth) * scale : target;
            pointer.plateau = Math.max(-plateauHalfWidth, Math.min(plateauHalfWidth, end));
        }
        const unclamped = value;
        value = clamp(value, limits);
        if (value !== before) this.update(this.active.control, this.valueFromDisplay(this.active.control, value));
        // Do not accumulate invisible travel beyond a hard bound.
        if (value !== unclamped) this.anchorPointer();
    }
    moveBy(pointerId: number, delta: number): void {
        const pointer = this.active?.pointer;
        if (pointer?.id === pointerId) this.move(pointerId, pointer.position + delta);
    }
    changeSensitivity(delta: number): boolean {
        const pointer = this.active?.pointer;
        if (!pointer) return false;
        if (!delta) return true;
        this.anchorPointer();
        pointer.speed = Math.max(-10, Math.min(10, pointer.speed - Math.sign(delta)));
        this.changed();
        return true;
    }
    private anchorPointer(): void {
        const pointer = this.active?.pointer;
        if (!pointer || !this.active) return;
        pointer.anchorPosition = pointer.position;
        pointer.anchorValue = this.displayValue(this.active.control, this.active.value);
        pointer.anchorPlateau = pointer.plateau;
    }
    isEditingNode(address: NodeAddress): boolean {
        return [this.active?.control, this.entry?.control].some((control) =>
            control && addressKey(control.node) === addressKey(address));
    }
    openEntry(control: ManualControlId, descriptor: ManualControlDescriptor, limits: ManualControlLimits,
        anchor: ManualValueEntry["anchor"]): void {
        this.finish();
        if (!descriptor.available || !descriptor.enabled) return;
        this.entry = { id: this.newId(), control, descriptor, limits, anchor,
            draft: String(this.displayValue(control, this.state(control)?.value ?? descriptor.value)), error: "" };
        this.changed();
    }
    numericEntry(): ManualValueEntry | null { return this.entry ? { ...this.entry } : null; }
    setEntryDraft(draft: string): void {
        if (!this.entry) return;
        this.entry.draft = draft; this.entry.error = ""; this.changed();
    }
    acceptEntry(): boolean {
        const entry = this.entry;
        if (!entry) return false;
        const value = Number(entry.draft);
        if (!entry.draft.trim() || !Number.isFinite(value)
            || value !== clamp(value, entry.limits)) {
            entry.error = "Enter a finite value within the input limits.";
            this.changed(); return false;
        }
        this.entry = null;
        this.edit(entry.control, entry.descriptor);
        this.update(entry.control, this.valueFromDisplay(entry.control, value));
        this.finish(entry.control);
        this.changed(); return true;
    }
    cancelEntry(): void {
        if (!this.entry) return;
        this.entry = null; this.changed();
    }
    resetHovered(): boolean {
        if (this.active?.pointer && this.active.pointer.limits.defaultValue !== undefined) {
            this.update(this.active.control, this.valueFromDisplay(this.active.control,
                clamp(this.active.pointer.limits.defaultValue, this.active.pointer.limits)));
            this.active.pointer.plateau = 0;
            this.anchorPointer();
            return true;
        }
        const hover = this.hoverDetails;
        if (!hover || !hover.descriptor.available || !hover.descriptor.enabled
            || hover.limits.defaultValue === undefined) return false;
        const value = clamp(hover.limits.defaultValue, hover.limits);
        if (this.displayValue(hover.control, this.state(hover.control)?.value ?? hover.descriptor.value) === value) return true;
        this.edit(hover.control, hover.descriptor);
        this.update(hover.control, this.valueFromDisplay(hover.control, value));
        this.finish(hover.control);
        return true;
    }
    invertOutput(control: ManualControlId, descriptor: ManualControlDescriptor): boolean {
        if (control.outputName === undefined || !descriptor.available || !descriptor.enabled || this.active?.pointer) return false;
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
        if (this.entry) return { text: `${manualControlName(this.entry.control)} · `
            + (this.entry.error ? `${this.entry.error} · Esc cancel` : "Enter apply · Esc cancel") };
        if (this.active?.pointer) return {
            text: `${manualControlName(this.active.control)} ${displayValue(this.displayValue(this.active.control, this.active.value))}`
                + (this.outputInverted(this.active.control, this.active.value) ? " · inverted" : "")
                + ` · ${displayValue(2 ** this.active.pointer.speed)}× · Wheel ↑×2 ↓½ · Esc release`,
        };
        if (!this.hoverDetails?.descriptor.available || !this.hoverDetails.descriptor.enabled) return null;
        const reset = this.hoverDetails.limits.defaultValue !== undefined;
        const { control, descriptor } = this.hoverDetails;
        const value = this.state(control)?.value ?? this.liveValue(control) ?? descriptor.value;
        return { text: manualKnobStatus(manualControlName(control), this.displayValue(control, value), reset) };
    }
    pointerControl(): ManualControlId | null { return this.active?.pointer ? this.active.control : null; }
    pointerId(): number | null { return this.active?.pointer?.id ?? null; }
    release(pointerId: number): void { if (this.active?.pointer?.id === pointerId) this.finish(); }
    finish(control?: ManualControlId): void {
        const active = this.active;
        if (!active || (control && manualControlKey(control) !== manualControlKey(active.control))) return;
        this.active = null;
        this.ending.set(manualControlKey(active.control), {
            interactionId: active.interactionId, control: active.control, value: active.value, phase: "ending",
        });
        this.send({ action: "end", interactionId: active.interactionId, value: active.value });
        this.changed();
    }
    disconnected(): void { this.active = null; this.hovered = null; this.hoverDetails = null; this.entry = null; this.remote = []; this.live = []; this.ending.clear(); this.changed(); }
}
