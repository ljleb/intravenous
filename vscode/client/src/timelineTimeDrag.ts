import { automationSampleIndex } from "./automationCurveGeometry";
import { ScrubSensitivity } from "./scrubSensitivity";

export function timelineTimeDrag(anchor: bigint, secondsPerPixel: number, sampleRate: number,
    targets: Iterable<bigint>, anchorPlateau = 0, anchorPhase = 0) {
    const samplesPerPixel = secondsPerPixel * sampleRate;
    const offsets = new Map<number, bigint>();
    for (const target of targets) {
        offsets.set((Number(BigInt.asUintN(64, target - anchor)) - anchorPhase) / samplesPerPixel, target);
        offsets.set((-Number(BigInt.asUintN(64, anchor - target)) - anchorPhase) / samplesPerPixel, target);
    }
    const positions = [...offsets.keys()].sort((a, b) => a - b);
    const scrub = new ScrubSensitivity(1, 0, 0);
    return (pixels: number) => {
        const detent = scrub.valueAtDetents(pixels, positions, anchorPlateau);
        const snappedIndex = detent.snappedValue === undefined ? undefined : offsets.get(detent.snappedValue);
        const delta = automationSampleIndex(detent.value * secondsPerPixel, sampleRate, anchorPhase / sampleRate) ?? 0n;
        // Keep ties on the same side of the rounding boundary after a rebase.
        const halfSample = .5 - Number.EPSILON;
        const phase = snappedIndex === undefined && Number.isSafeInteger(Number(delta))
            ? Math.max(-halfSample, Math.min(halfSample, detent.value * samplesPerPixel + anchorPhase - Number(delta))) : 0;
        return { sampleIndex: snappedIndex ?? BigInt.asIntN(64, anchor + delta), snappedIndex,
            offset: detent.value, plateau: detent.plateau, phase };
    };
}

export function playheadTimeDrag(anchor: bigint, secondsPerPixel: number, sampleRate: number,
    visible: readonly bigint[], nearby?: (sample: bigint, forward: boolean) => readonly bigint[] | undefined) {
    let atTravel = timelineTimeDrag(anchor, secondsPerPixel, sampleRate, visible);
    let travel = 0;
    let pickup = 0;
    let previous = atTravel(0);
    let loaded: readonly bigint[] = [];
    return (pixels: number) => {
        let next = atTravel(pixels - pickup);
        const targets = nearby?.(next.sampleIndex, pixels >= travel);
        if (targets && targets !== loaded && (targets.length !== loaded.length || targets.some((target, i) => target !== loaded[i]))) {
            loaded = targets;
            const combined = new Set([...visible, ...loaded]);
            // New targets apply from the last pointer position. Keep any
            // travel already spent inside a detent when a window rolls over.
            if (previous.snappedIndex !== undefined) combined.add(previous.snappedIndex);
            atTravel = timelineTimeDrag(previous.sampleIndex, secondsPerPixel, sampleRate, combined, previous.plateau, previous.phase);
            pickup = travel;
            next = atTravel(pixels - pickup);
        }
        travel = pixels;
        return previous = next;
    };
}
