import { detentHalfWidth, ScrubSensitivity } from "./scrubSensitivity";
import type { KnobGeometry } from "./graphModel";

export type KnobLimits = { min?: number; max?: number; defaultValue?: number; scrubSpeed?: number;
    wholeByDefault?: boolean; integer?: boolean; geometry?: KnobGeometry };

export function circularKnobRange({ min, max, geometry }: KnobLimits): { min: number; span: number } | undefined {
    if (geometry !== "circle" || min === undefined || max === undefined || !Number.isFinite(min) ||
        !Number.isFinite(max) || max <= min || !Number.isFinite(max - min)) return undefined;
    return { min, span: max - min };
}

export function normalizeKnobValue(value: number, limits: KnobLimits): number {
    const { min = -Infinity, max = Infinity } = limits;
    const range = circularKnobRange(limits);
    if (range && Number.isFinite(value)) {
        if (min <= value && value < max) return value;
        const { span } = range;
        const remainder = ((value % span) - (min % span)) % span;
        const wrapped = min + (remainder < 0 ? remainder + span : remainder);
        return wrapped < max ? wrapped : min;
    }
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
        if (this.limits.defaultValue !== limits.defaultValue || this.limits.min !== limits.min ||
            this.limits.max !== limits.max || this.limits.geometry !== limits.geometry)
            this.anchor(value);
        this.limits = limits;
    }
    changeSensitivity(delta: number, value: number): boolean {
        if (!this.sensitivity.change(delta, this.position, value)) return false;
        this.anchorPlateau = this.plateau;
        return true;
    }
    private moveCircular(position: number, limits: KnobLimits, span: number): number {
        let value = this.sensitivity.valueAt(position);
        if (limits.defaultValue !== undefined && Number.isFinite(limits.defaultValue)) {
            const target = normalizeKnobValue(limits.defaultValue, limits);
            const speed = this.sensitivity.unitsPerPixel;
            const anchor = normalizeKnobValue(this.sensitivity.anchorValue - target, { geometry: "circle", min: 0, max: span });
            const detentWidth = 2 * detentHalfWidth;
            const anchorPixels = anchor === 0 ? this.anchorPlateau + detentHalfWidth : anchor / speed + detentWidth;
            const periodPixels = span / speed + detentWidth;
            // In pointer space each turn includes the full width of its
            // default detent. This is independent of pointer event frequency.
            const phase = normalizeKnobValue(anchorPixels + (position - this.sensitivity.anchorPosition),
                { geometry: "circle", min: 0, max: periodPixels });
            this.plateau = Math.min(phase, detentWidth) - detentHalfWidth;
            if (phase <= detentWidth) return target;
            value = target + (phase - detentWidth) * speed;
        }
        if (limits.wholeByDefault || limits.integer) {
            const step = limits.integer ? 1 : Math.min(1, this.sensitivity.multiplier);
            value = Math.round(value / step) * step;
        }
        return normalizeKnobValue(value, limits);
    }
    move(position: number, current: number, limits = this.limits): number {
        if (position === this.position && limits === this.limits) return current;
        this.position = position;
        const circular = circularKnobRange(limits);
        if (circular) return this.moveCircular(position, limits, circular.span);
        let value = this.sensitivity.valueAt(position);
        const target = limits.defaultValue;
        const hasDetent = target !== undefined && Number.isFinite(target) && normalizeKnobValue(target, limits) === target;
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
        const clamped = normalizeKnobValue(value, limits);
        // The exact default can sit between quantized values. Rounding must
        // keep the result on the same side of the detent as the drag.
        const result = hasDetent && (unrounded - target) * (clamped - target) < 0 ? target : clamped;
        if (value !== clamped) this.anchor(result);
        return result;
    }
}
