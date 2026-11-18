import type { ManualControlLimits } from "./manualControlGesture";

// All scale choices depend on the declaration, never the knob's current value.
export function knobScrubSpeed({ min, max, defaultValue = 0, scrubSpeed }: ManualControlLimits): number {
    if (scrubSpeed !== undefined && Number.isFinite(scrubSpeed) && scrubSpeed > 0) return scrubSpeed;
    if (min !== undefined && max !== undefined && max > min) return (max - min) / 200;
    return Math.max(Math.abs(defaultValue), 1) / 100;
}

export function knobFraction(value: number, { min, max, defaultValue = 0 }: ManualControlLimits): number {
    if (min !== undefined && max !== undefined)
        return max > min ? Math.max(0, Math.min(1, (value - min) / (max - min))) : .5;
    if (min !== undefined)
        return 2 * Math.atan(Math.max(0, value - min) / Math.max(Math.abs(defaultValue - min), 1)) / Math.PI;
    if (max !== undefined)
        return 1 - 2 * Math.atan(Math.max(0, max - value) / Math.max(Math.abs(max - defaultValue), 1)) / Math.PI;
    return .5 + Math.atan((value - defaultValue) / Math.max(Math.abs(defaultValue), 1)) / Math.PI;
}

export function knobMagnitude(value: number, { min, max, defaultValue = 0 }: ManualControlLimits): number {
    const offset = value - defaultValue;
    if (offset === 0 || (offset < 0 ? min !== undefined : max !== undefined)) return 0;
    return Math.min(4, Math.log10(1 + Math.abs(offset) / Math.max(Math.abs(defaultValue), 1)));
}
