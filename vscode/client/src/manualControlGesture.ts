import type { ManualControlAction, ManualControlDescriptor, ManualControlId, ManualControlInteractionState } from "./manualControls";
import { manualControlKey } from "./manualControls";

type Gesture = {
    interactionId: string; control: ManualControlId; value: number;
    pointer?: { id: number; x: number; value: number; unitsPerPixel: number };
};
// Owned by the webview root, never by a presentation widget. Components can
// disappear while these window-level pointer and keyboard gestures continue.
export class ManualControlGestures {
    private active: Gesture | null = null;
    private hovered: ManualControlId | null = null;
    private remote: ManualControlInteractionState[] = [];
    constructor(private readonly send: (action: ManualControlAction) => void,
        private readonly newId: () => string, private readonly changed: () => void) {}
    state(control: ManualControlId): ManualControlInteractionState | undefined {
        if (this.active && manualControlKey(this.active.control) === manualControlKey(control))
            return { ...this.active, phase: "active" };
        return this.remote.find((entry) => manualControlKey(entry.control) === manualControlKey(control));
    }
    receive(states: ManualControlInteractionState[]): void {
        this.remote = states;
        if (this.active && states.some((entry) => entry.interactionId === this.active?.interactionId && entry.phase === "failed"))
            this.active = null;
        this.changed();
    }
    hover(control: ManualControlId | null): void { this.hovered = control; this.send({ action: "hover", control }); }
    unhover(control: ManualControlId): void {
        if (this.hovered && manualControlKey(this.hovered) === manualControlKey(control)) this.hover(null);
    }
    edit(control: ManualControlId, descriptor: ManualControlDescriptor): void {
        if (this.active && manualControlKey(this.active.control) === manualControlKey(control)) return;
        this.finish();
        const value = this.state(control)?.value ?? descriptor.value;
        if (!Number.isFinite(value) || !descriptor.available || !descriptor.enabled) return;
        this.active = { interactionId: this.newId(), control, value };
        this.send({ action: "begin", ...this.active });
        this.changed();
    }
    scrub(control: ManualControlId, descriptor: ManualControlDescriptor,
        pointerId: number, x: number, unitsPerPixel: number): void {
        this.edit(control, descriptor);
        if (!this.active) return;
        this.active.pointer = { id: pointerId, x, value: this.active.value, unitsPerPixel };
    }
    update(control: ManualControlId, value: number): void {
        if (!this.active || manualControlKey(this.active.control) !== manualControlKey(control) || !Number.isFinite(value)) return;
        this.active.value = value;
        this.send({ action: "update", interactionId: this.active.interactionId, value });
        this.changed();
    }
    move(pointerId: number, x: number, min?: number, max?: number): void {
        const pointer = this.active?.pointer;
        if (!pointer || pointer.id !== pointerId || !this.active) return;
        let value = pointer.value + (x - pointer.x) * pointer.unitsPerPixel;
        if (min !== undefined) value = Math.max(min, value);
        if (max !== undefined) value = Math.min(max, value);
        this.update(this.active.control, value);
    }
    pointerControl(): ManualControlId | null { return this.active?.pointer ? this.active.control : null; }
    release(pointerId: number): void { if (this.active?.pointer?.id === pointerId) this.finish(); }
    finish(control?: ManualControlId): void {
        const active = this.active;
        if (!active || (control && manualControlKey(control) !== manualControlKey(active.control))) return;
        this.active = null;
        this.send({ action: "end", interactionId: active.interactionId, value: active.value });
        this.changed();
    }
    disconnected(): void { this.active = null; this.hovered = null; this.remote = []; this.changed(); }
}
