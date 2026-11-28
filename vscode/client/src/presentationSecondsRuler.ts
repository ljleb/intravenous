import type { PresentationMark, PresentationSnapshot } from "./presentationPlugin";
import { timelineTicksWithSampleDivisions } from "./timelineGeometry";
import type { PresentationTime } from "./timelineSamples";

export function renderPresentationSecondsRuler(time: PresentationTime, width: number, height: number,
    mode: PresentationSnapshot["mode"], inset: number): PresentationMark[] {
    if (time.gridMode && time.gridMode !== "seconds") return [];
    const ticks = timelineTicksWithSampleDivisions(time, width);
    const count = Math.min(240, ticks.length);
    const showLabels = mode !== "nano" && (mode !== "compact" || height > 16);
    const marks: PresentationMark[] = [];
    for (let i = 0; i < count; ++i) {
        const index = count > 1 ? Math.round(i * (ticks.length - 1) / (count - 1)) : 0;
        const { x, minor, label, sampleLabel } = ticks[index];
        marks.push({ type: "polyline", id: `seconds-ruler:${index}`, points: [{ x, y: 0 }, { x, y: height }],
            ink: "foreground", opacity: minor ? .09 : .3 });
        if (!showLabels) continue;
        const labelX = index === ticks.length - 1 && x >= width - 48
            ? Math.max(inset, x - 3 - Math.max(label.length, sampleLabel?.length ?? 0) * 6) : Math.max(inset, x) + 3;
        if (!minor) marks.push({ type: "text", id: `seconds-label:${index}`, text: label, x: labelX, y: 10, ink: "muted" });
        if (sampleLabel) marks.push({ type: "text", id: `seconds-samples:${index}`, text: sampleLabel, x: labelX, y: 21, ink: "muted" });
    }
    return marks;
}
