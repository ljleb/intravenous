import type { MeterMode, ObservationDisplayValue } from "./observations";

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
export function meterReading(mode: MeterMode, result?: ObservationDisplayValue): MeterReading {
    const missing: MeterReading = { available: false, position: 0, rmsPosition: 0,
        below: false, above: false, overReference: false, text: "Unavailable" };
    if (!result?.available) return missing;
    const peak = result.value?.peak, rms = result.value?.rms;
    const samples = result.value?.samples;
    const hasSamples = typeof samples === "string" ? /^[1-9]\d*$/.test(samples)
        : typeof samples === "number" && Number.isSafeInteger(samples) && samples > 0;
    if (!number(peak) || !number(rms) || peak < 0 || rms < 0 || !hasSamples) {
        const count = result.value?.nonfinite;
        return count && count !== "0" ? { ...missing, text: `Unavailable · ${count} nonfinite samples` } : missing;
    }
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
