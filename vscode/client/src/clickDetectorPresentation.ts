import type { Present, PresentationManifest, PresentationMark } from "./presentationPlugin";
import { presentationPluginVersion } from "./presentationPlugin";
import { recordedTriggerBinsOperation } from "./observations";
import { timelineSamples } from "./timelineSamples";

export const clickDetectorPresentationManifest: PresentationManifest = {
    version: presentationPluginVersion,
    id: "builtin.click_detector",
    title: "Click Detector",
    definitionIds: ["builtin.click_detector"],
    timeline: true,
    icon: "M1 8h4l2-5 2 10 2-5h4",
    observations: [{ id: "clicks", direction: "output", port: "clicks", operation: recordedTriggerBinsOperation }],
};

export function createClickDetectorPresentation(): Present {
    return ({ viewport: { time, width, height }, observations, mode, node }) => {
        const connected = node.ports.some((port) => port.name === "audio" && port.direction === "input" && port.connected);
        const rate = time?.sampleRate;
        const observation = observations.clicks;
        const waiting = connected ? "Waiting for click observations" : "Connect the audio to inspect";
        if (!time || !rate || !observation) return { status: waiting };
        const view = timelineSamples(time);
        const visibleOffset = view.origin.fraction > 0 ? 1 - view.origin.fraction : 0;
        const pixelsPerSample = width / view.span;
        const columns = new Set<number>();
        let count = 0n;
        let partial = false;
        for (const span of observation.spans) {
            if (span.result.status !== "ready") continue;
            const begin = BigInt(span.begin), end = BigInt(span.end);
            const windowBegin = BigInt(span.result.window.begin);
            const base = BigInt(span.windowOffset);
            for (const bin of span.result.triggerBins ?? []) {
                const binBegin = base + BigInt.asUintN(64, BigInt(bin.begin) - windowBegin);
                const binEnd = binBegin + BigInt(bin.length);
                if (binEnd <= begin || binBegin >= end) continue;
                const complete = binBegin >= begin && binEnd <= end;
                partial ||= !complete;
                if (complete) count += BigInt(bin.count);
                const offset = binBegin + BigInt.asUintN(64, BigInt(bin.first) - BigInt(bin.begin));
                if (offset < begin || offset >= end) continue;
                const x = (Number(offset) + visibleOffset) * pixelsPerSample;
                if (x < 0 || x > width) continue;
                if (!complete) ++count;
                columns.add(Math.min(Math.floor(x), Math.max(0, Math.ceil(width) - 1)));
            }
        }
        const marks = Array.from(columns, (x): PresentationMark =>
            ({ id: `click:${x}`, type: "rect", ink: "warning", x, y: 0, width: 1, height }));
        const status = count || observation.result?.status === "ready"
            ? `${partial ? "At least " : ""}${count} recorded ${count === 1n ? "click" : "clicks"} in view`
            : observation.error ?? (observation.status === "unavailable" ? "Click observation is unavailable" : waiting);
        if (mode === "editable") marks.push({ id: "count", type: "text", ink: "muted",
            x: Math.max(4, width - status.length * 7 - 8), y: 16, text: status });
        return { marks, status };
    };
}
