import type { MeterMode, ObservationDisplayValue } from "./observations";
import { MeterBallistics, type MeterMotion } from "./meterBallistics";

export const levelDomain = { min: -60, max: 6 } as const;
export const valueDomain = { min: 0, max: 1 } as const;
export function sampleDecibels(amplitude: number): number {
    return amplitude > 0 ? 20 * Math.log10(amplitude) : -Infinity;
}
export function meterPosition(value: number, min: number, max: number): number {
    return Math.max(0, Math.min(1, (value - min) / (max - min)));
}
export type MeterReading = {
    available: boolean;
    position: number;
    rmsPosition: number;
    displayValue?: number;
    below: boolean;
    above: boolean;
    overReference: boolean;
    text: string;
};
function number(value: unknown): value is number { return typeof value === "number" && Number.isFinite(value); }
export function meterReading(mode: MeterMode, result?: ObservationDisplayValue, gain = 1): MeterReading {
    const missing: MeterReading = { available: false, position: 0, rmsPosition: 0,
        below: false, above: false, overReference: false, text: "Unavailable" };
    if (!result?.available) return missing;
    const rawPeak = result.value?.peak, rawRms = result.value?.rms;
    const samples = result.value?.samples;
    const hasSamples = typeof samples === "string" ? /^[1-9]\d*$/.test(samples)
        : typeof samples === "number" && Number.isSafeInteger(samples) && samples > 0;
    if (!number(rawPeak) || !number(rawRms) || rawPeak < 0 || rawRms < 0 || !hasSamples) {
        const count = result.value?.nonfinite;
        return count && count !== "0" ? { ...missing, text: `Unavailable · ${count} nonfinite samples` } : missing;
    }
    const magnitude = Number.isFinite(gain) ? Math.abs(gain) : 1;
    const peak = rawPeak * magnitude, rms = rawRms * magnitude;
    if (mode === "value") {
        const nonfinite = result.value?.nonfinite;
        return { available: true, position: meterPosition(peak, 0, 1), rmsPosition: meterPosition(rms, 0, 1),
            displayValue: peak, below: false, above: peak > 1, overReference: peak > 1,
            text: `${peak} peak · ${rms} RMS`
                + (nonfinite && nonfinite !== "0" ? ` · ${nonfinite} nonfinite samples` : "") };
    }
    const db = sampleDecibels(peak), rmsDb = sampleDecibels(rms);
    const label = (value: number) => value === -Infinity ? "−∞" : `${value > 0 ? "+" : ""}${value.toFixed(1)}`;
    const nonfinite = result.value?.nonfinite;
    return { available: true, position: meterPosition(db, levelDomain.min, levelDomain.max),
        rmsPosition: meterPosition(rmsDb, levelDomain.min, levelDomain.max), displayValue: db,
        below: db < levelDomain.min, above: db > levelDomain.max, overReference: peak > 1,
        text: `${label(db)} dBFS peak · ${label(rmsDb)} dBFS RMS`
            + (nonfinite && nonfinite !== "0" ? ` · ${nonfinite} nonfinite samples` : "") };
}

// Owns one compatible meter's presentation history, independent of executable
// lifetime. Only copied telemetry survives; no old frame is accepted as current.
export class MeterPresentationState {
    private readonly motion = new MeterBallistics();
    private displayed: MeterMotion = { peak: 0 };
    private current?: ObservationDisplayValue;
    private previous?: ObservationDisplayValue;
    private active = false;

    update(result: ObservationDisplayValue | undefined, active: boolean, now: number, retained?: ObservationDisplayValue): void {
        this.current = result;
        this.active = active;
        const available = meterReading("value", result).available;
        if (!active || (result !== undefined && !result.pending && !available)) {
            this.previous = undefined;
            this.displayed = { peak: 0 };
        } else if (available) {
            const peak = result!.value!.peak as number;
            this.displayed = this.previous ? this.motion.update(peak, now) : this.motion.reset(peak, now);
            this.previous = result;
        } else if (!this.previous && retained && meterReading("value", retained).available) {
            this.previous = retained;
            this.displayed = this.motion.reset(retained.value!.peak as number, now);
        } else this.advance(now);
    }
    advance(now: number): void {
        if (this.active && this.previous) this.displayed = this.motion.advance(now);
    }
    get moving(): boolean { return this.active && !!this.previous && this.motion.moving; }
    get updating(): boolean { return this.active && !!this.previous && (!this.current || !!this.current.pending); }
    reading(mode: MeterMode, gain = 1): MeterReading & { heldPosition: number; heldOverReference: boolean } {
        const result = !this.active ? undefined : this.updating ? this.previous : this.current;
        // The filled bar follows received peaks immediately in both directions.
        // Only the separate held marker uses presentation motion.
        const reading = meterReading(mode, result, gain);
        const held = meterReading(mode, result && { ...result,
            value: { ...result.value, peak: this.displayed.peak } }, gain);
        return { ...reading, heldPosition: held.position, heldOverReference: held.overReference };
    }
    status(mode: MeterMode, gain = 1): string {
        return this.updating ? `Updating · previous reading: ${meterReading(mode, this.previous, gain).text}`
            : meterReading(mode, this.active ? this.current : undefined, gain).text;
    }
}
