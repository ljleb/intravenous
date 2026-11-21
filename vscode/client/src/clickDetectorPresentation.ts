import type { Present, PresentationManifest, PresentationMark } from "./presentationPlugin";
import { recordedEventTimestampsOperation } from "./observations";
import { sampleTimeSpan } from "./sampleWaveform";

export const clickDetectorPresentationManifest: PresentationManifest = {
    version: 1,
    id: "builtin.click_detector",
    title: "Click Detector",
    definitionIds: ["builtin.click_detector"],
    timeline: true,
    icon: "M1 8h4l2-5 2 10 2-5h4",
    observations: [{ id: "clicks", direction: "output", port: "clicks", operation: recordedEventTimestampsOperation }],
};

export function createClickDetectorPresentation(): Present {
    return ({ viewport: { time, width, height }, observations, mode, node }) => {
        const connected = node.ports.some((port) => port.name === "audio" && port.direction === "input" && port.connected);
        const rate = time?.sampleRate;
        const observation = observations.clicks;
        const waiting = connected ? "Waiting for click observations" : "Connect the audio to inspect";
        if (!time || !rate || !observation) return { status: waiting };
        const end = time.beginSeconds + time.spanSeconds;
        const window = sampleTimeSpan(time.beginSeconds, end, rate);
        if (!window) return {};
        const origin = BigInt(window.begin);
        const subSampleOffset = Number(origin) - time.beginSeconds * rate;
        const pixelsPerSample = width / (time.spanSeconds * rate);
        const marks: PresentationMark[] = [];
        for (const span of observation.spans) {
            if (span.result.status !== "ready") continue;
            const begin = BigInt(span.begin), end = BigInt(span.end);
            let previousIndex: string | undefined;
            let occurrence = 0;
            for (const index of span.result.eventIndices ?? []) {
                const offset = BigInt.asIntN(64, BigInt(index) - origin);
                if (offset < begin || offset >= end) continue;
                const x = (Number(offset) + subSampleOffset) * pixelsPerSample;
                if (x < 0 || x >= width) continue;
                occurrence = previousIndex === index ? occurrence + 1 : 0;
                previousIndex = index;
                marks.push({ id: `click:${index}:${occurrence}`, type: "rect", ink: "warning", x, y: 0, width: 1, height });
            }
        }
        const count = marks.length;
        const status = count || observation.result?.status === "ready"
            ? `${count} recorded ${count === 1 ? "click" : "clicks"} in view`
            : observation.error ?? (observation.status === "unavailable" ? "Click observation is unavailable" : waiting);
        if (mode === "editable") marks.push({ id: "count", type: "text", ink: "muted",
            x: Math.max(4, width - status.length * 7 - 8), y: 16, text: status });
        return { marks, status };
    };
}
