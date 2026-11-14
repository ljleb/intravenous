import type { ManualControlAction, ManualControlDescriptor, ManualControlId, ManualControlInteractionState } from "./manualControls";
import { manualControlKey } from "./manualControls";
import { addressKey, type NodeAddress } from "./graphScope";

export type ManualControlLimits = { min?: number; max?: number; defaultValue?: number };
export type ManualControlHint = { text: string; tooltip: string };
type Hover = { control: ManualControlId; descriptor: ManualControlDescriptor; limits: ManualControlLimits };
export type ManualValueEntry = Hover & {
    id: string; draft: string; error: string;
    anchor: { left: number; top: number; bottom: number };
};

type Gesture = {
    interactionId: string; control: ManualControlId; value: number;
    pointer?: { id: number; position: number; initialScale: number; speed: number;
        plateau: number; limits: ManualControlLimits };
};
const plateauHalfWidth = 6; // Extra pixels on either side of the exact default.
const clamp = (value: number, limits: ManualControlLimits) =>
    Math.max(limits.min ?? -Infinity, Math.min(limits.max ?? Infinity, value));
const displayValue = (value: number) => Number(value.toPrecision(6)).toString();
// Owned by the webview root, never by a presentation widget. Components can
// disappear while these window-level pointer and keyboard gestures continue.
export class ManualControlGestures {
    private active: Gesture | null = null;
    private hovered: ManualControlId | null = null;
    private hoverDetails: Hover | null = null;
    private entry: ManualValueEntry | null = null;
    private remote: ManualControlInteractionState[] = [];
    private readonly ending = new Map<string, ManualControlInteractionState>();
    constructor(private readonly send: (action: ManualControlAction) => void,
        private readonly newId: () => string, private readonly changed: () => void) {}
    state(control: ManualControlId): ManualControlInteractionState | undefined {
        if (this.active && manualControlKey(this.active.control) === manualControlKey(control))
            return { ...this.active, phase: "active" };
        return this.ending.get(manualControlKey(control))
            ?? this.remote.find((entry) => manualControlKey(entry.control) === manualControlKey(control));
    }
    receive(states: ManualControlInteractionState[]): void {
        this.remote = states;
        for (const [key, pending] of this.ending) {
            if (states.some((entry) => entry.interactionId === pending.interactionId
                && (entry.phase === "settled" || entry.phase === "failed"))) this.ending.delete(key);
        }
        if (this.active && states.some((entry) => entry.interactionId === this.active?.interactionId && entry.phase === "failed"))
            this.active = null;
        this.changed();
    }
    hover(control: ManualControlId | null, descriptor?: ManualControlDescriptor, limits: ManualControlLimits = {}): void {
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
        let changed = false;
        if (this.hoverDetails && manualControlKey(this.hoverDetails.control) === key) {
            this.hoverDetails = { control, descriptor, limits }; changed = true;
        }
        if (this.entry && manualControlKey(this.entry.control) === key) {
            this.entry.descriptor = descriptor; this.entry.limits = limits; changed = true;
        }
        if (this.active?.pointer && manualControlKey(this.active.control) === key) {
            if (this.active.pointer.limits.defaultValue !== limits.defaultValue) this.active.pointer.plateau = 0;
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
        this.active = { interactionId: this.newId(), control, value };
        this.send({ action: "begin", ...this.active });
        this.changed();
    }
    scrub(control: ManualControlId, descriptor: ManualControlDescriptor,
        pointerId: number, position: number, unitsPerPixel: number, limits: ManualControlLimits = {}): void {
        this.cancelEntry();
        this.edit(control, descriptor);
        if (!this.active || !Number.isFinite(unitsPerPixel) || unitsPerPixel <= 0) return;
        this.active.pointer = { id: pointerId, position, initialScale: unitsPerPixel, speed: 0, plateau: 0, limits };
        this.changed();
    }
    update(control: ManualControlId, value: number): void {
        if (!this.active || manualControlKey(this.active.control) !== manualControlKey(control) || !Number.isFinite(value)) return;
        this.active.value = value;
        this.send({ action: "update", interactionId: this.active.interactionId, value });
        this.changed();
    }
    move(pointerId: number, position: number, min?: number, max?: number): void {
        const pointer = this.active?.pointer;
        if (!pointer || pointer.id !== pointerId || !this.active) return;
        const delta = position - pointer.position;
        pointer.position = position;
        const limits = { ...pointer.limits, min: min ?? pointer.limits.min, max: max ?? pointer.limits.max };
        const scale = pointer.initialScale * 2 ** pointer.speed;
        const before = this.active.value;
        const target = limits.defaultValue;
        let value = before + delta * scale;
        if (target !== undefined && Number.isFinite(target) && clamp(target, limits) === target) {
            // Numeric values occupy their full range. Only the exact default
            // gets extra physical travel, retained across sensitivity changes.
            const start = before < target ? (before - target) / scale - plateauHalfWidth
                : before > target ? (before - target) / scale + plateauHalfWidth : pointer.plateau;
            const end = start + delta;
            value = end < -plateauHalfWidth ? target + (end + plateauHalfWidth) * scale
                : end > plateauHalfWidth ? target + (end - plateauHalfWidth) * scale : target;
            pointer.plateau = Math.max(-plateauHalfWidth, Math.min(plateauHalfWidth, end));
        }
        value = clamp(value, limits);
        if (value !== before) this.update(this.active.control, value);
    }
    moveBy(pointerId: number, delta: number): void {
        const pointer = this.active?.pointer;
        if (pointer?.id === pointerId) this.move(pointerId, pointer.position + delta);
    }
    changeSensitivity(delta: number): boolean {
        const pointer = this.active?.pointer;
        if (!pointer) return false;
        if (!delta) return true;
        pointer.speed = Math.max(-10, Math.min(10, pointer.speed - Math.sign(delta)));
        this.changed();
        return true;
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
            draft: String(this.state(control)?.value ?? descriptor.value), error: "" };
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
        this.update(entry.control, value);
        this.finish(entry.control);
        this.changed(); return true;
    }
    cancelEntry(): void {
        if (!this.entry) return;
        this.entry = null; this.changed();
    }
    resetHovered(): boolean {
        if (this.active?.pointer && this.active.pointer.limits.defaultValue !== undefined) {
            this.update(this.active.control, clamp(this.active.pointer.limits.defaultValue, this.active.pointer.limits));
            this.active.pointer.plateau = 0;
            return true;
        }
        const hover = this.hoverDetails;
        if (!hover || !hover.descriptor.available || !hover.descriptor.enabled
            || hover.limits.defaultValue === undefined) return false;
        const value = clamp(hover.limits.defaultValue, hover.limits);
        if ((this.state(hover.control)?.value ?? hover.descriptor.value) === value) return true;
        this.edit(hover.control, hover.descriptor);
        this.update(hover.control, value);
        this.finish(hover.control);
        return true;
    }
    hint(): ManualControlHint | null {
        if (this.entry) return { text: `${this.entry.control.inputName}: Enter accept · Esc cancel`,
            tooltip: "Type an exact value. Enter accepts; Escape or clicking outside cancels. Drag snapping does not affect numeric entry." };
        if (this.active?.pointer) return {
            text: `${this.active.control.inputName} ${displayValue(this.active.value)} · ${displayValue(2 ** this.active.pointer.speed)}×`,
            tooltip: "Drag up to increase or down to decrease. Scroll down for finer control; scroll up for faster control."
                + " Each wheel step halves or doubles sensitivity. Release to finish; Escape releases the pointer.",
        };
        if (!this.hoverDetails?.descriptor.available || !this.hoverDetails.descriptor.enabled) return null;
        const reset = this.hoverDetails.limits.defaultValue !== undefined;
        return { text: `${this.hoverDetails.control.inputName}: drag${reset ? " · double-click reset" : ""} · right-click value`,
            tooltip: "Drag up/down to adjust. While dragging, use the wheel to change sensitivity. Right-click for numeric entry."
                + (reset ? " Double-click to reset to the declared default." : "") + " Arrow keys adjust the focused knob." };
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
    disconnected(): void { this.active = null; this.hovered = null; this.hoverDetails = null; this.entry = null; this.remote = []; this.ending.clear(); this.changed(); }
}
