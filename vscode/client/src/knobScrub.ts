import { ScrubSensitivity } from "./scrubSensitivity";

export type KnobLimits = { min?: number; max?: number; defaultValue?: number; scrubSpeed?: number;
    wholeByDefault?: boolean; integer?: boolean };

export function clampKnobValue(value: number, { min = -Infinity, max = Infinity }: KnobLimits): number {
    return Math.max(min, Math.min(max, value));
}

export class KnobScrub {
    private readonly sensitivity: ScrubSensitivity;
    private plateau = 0;
    private anchorPlateau = 0;
    constructor(speed: number, public position: number, value: number, public limits: KnobLimits, precise = false) {
        this.sensitivity = new ScrubSensitivity(speed, position, value, precise);
    }
    hint(): string { return this.sensitivity.hint(); }
    anchor(value: number): void {
        this.sensitivity.anchorAt(this.position, value);
        this.anchorPlateau = this.plateau;
    }
    resetAnchor(value: number): void { this.plateau = 0; this.anchor(value); }
    refresh(limits: KnobLimits, value: number): void {
        if (this.limits.defaultValue !== limits.defaultValue) this.plateau = 0;
        if (this.limits.defaultValue !== limits.defaultValue || this.limits.min !== limits.min || this.limits.max !== limits.max)
            this.anchor(value);
        this.limits = limits;
    }
    changeSensitivity(delta: number, value: number): boolean {
        if (!this.sensitivity.change(delta, this.position, value)) return false;
        this.anchorPlateau = this.plateau;
        return true;
    }
    move(position: number, current: number, limits = this.limits): number {
        if (position === this.position && limits === this.limits) return current;
        this.position = position;
        let value = this.sensitivity.valueAt(position);
        const target = limits.defaultValue;
        const hasDetent = target !== undefined && Number.isFinite(target) && clampKnobValue(target, limits) === target;
        if (hasDetent) {
            const snap = this.sensitivity.valueAtDetent(target, position - this.sensitivity.anchorPosition, this.anchorPlateau);
            this.plateau = snap.plateau;
            if (snap.snappedValue !== undefined) return snap.snappedValue;
            value = snap.value;
        }
        const unrounded = value;
        if (limits.wholeByDefault || limits.integer) {
            const step = limits.integer ? 1 : Math.min(1, this.sensitivity.multiplier);
            const first = Math.ceil((limits.min ?? -Infinity) / step), last = Math.floor((limits.max ?? Infinity) / step);
            if (first <= last) {
                limits = { ...limits, min: first * step, max: last * step };
                value = Math.round(value / step) * step;
            }
        }
        const clamped = clampKnobValue(value, limits);
        // The exact default can sit between quantized values. Rounding must
        // keep the result on the same side of the detent as the drag.
        const result = hasDetent && (unrounded - target) * (clamped - target) < 0 ? target : clamped;
        if (value !== clamped) this.anchor(result);
        return result;
    }
}
