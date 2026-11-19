import { automationSampleIndex } from "./automationCurveGeometry";
import { ScrubSensitivity } from "./scrubSensitivity";

export function timelineTimeDrag(anchor: bigint, secondsPerPixel: number, sampleRate: number,
    targets: Iterable<bigint>, anchorPlateau = 0) {
    const samplesPerPixel = secondsPerPixel * sampleRate;
    const offsets = new Map<number, bigint>();
    for (const target of targets) {
        offsets.set(Number(BigInt.asUintN(64, target - anchor)) / samplesPerPixel, target);
        offsets.set(-Number(BigInt.asUintN(64, anchor - target)) / samplesPerPixel, target);
    }
    const positions = [...offsets.keys()].sort((a, b) => a - b);
    const scrub = new ScrubSensitivity(1, 0, 0);
    return (pixels: number) => {
        const detent = scrub.valueAtDetents(pixels, positions, anchorPlateau);
        const snappedIndex = detent.snappedValue === undefined ? undefined : offsets.get(detent.snappedValue);
        const delta = automationSampleIndex(detent.value * secondsPerPixel, sampleRate) ?? 0n;
        return { sampleIndex: snappedIndex ?? BigInt.asIntN(64, anchor + delta), snappedIndex,
            offset: detent.value, plateau: detent.plateau };
    };
}
