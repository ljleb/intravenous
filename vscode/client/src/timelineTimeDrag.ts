import { ScrubSensitivity } from "./scrubSensitivity";

export type TimelineDragScale = { spanSamples: number; width: number };

export function timelineTimeDrag(anchor: bigint, { spanSamples, width }: TimelineDragScale,
    targets: Iterable<bigint>, anchorPlateau = 0, anchorPhase = 0) {
    const samplesPerPixel = spanSamples / width;
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
        const travel = detent.value / width * spanSamples;
        const whole = Math.trunc(travel);
        const remainder = travel - whole + anchorPhase;
        let adjusted = Math.round(remainder);
        if (adjusted - remainder === .5 && whole + remainder < 0) --adjusted;
        const delta = BigInt(whole) + BigInt(adjusted);
        // Keep ties on the same side of the rounding boundary after a rebase.
        const halfSample = .5 - Number.EPSILON;
        const phase = snappedIndex === undefined ? Math.max(-halfSample, Math.min(halfSample, remainder - adjusted)) : 0;
        return { sampleIndex: snappedIndex ?? BigInt.asIntN(64, anchor + delta), snappedIndex,
            offset: detent.value, plateau: detent.plateau, phase };
    };
}

export function playheadTimeDrag(anchor: bigint, scale: TimelineDragScale,
    visible: () => readonly bigint[], nearby?: (sample: bigint, forward: boolean) => readonly bigint[] | undefined) {
    const sameTargets = (first: readonly bigint[], second: readonly bigint[]) => first === second
        || (first.length === second.length && first.every((target, index) => target === second[index]));
    let displayed = visible();
    let atTravel = timelineTimeDrag(anchor, scale, displayed);
    let travel = 0;
    let pickup = 0;
    let previous = atTravel(0);
    let loaded: readonly bigint[] = [];
    return (pixels: number) => {
        let next = atTravel(pixels - pickup);
        const nextDisplayed = visible();
        const nextLoaded = nearby?.(next.sampleIndex, pixels >= travel) ?? loaded;
        const targetsChanged = !sameTargets(displayed, nextDisplayed) || !sameTargets(loaded, nextLoaded);
        displayed = nextDisplayed;
        loaded = nextLoaded;
        if (targetsChanged) {
            const combined = new Set([...displayed, ...loaded]);
            // New targets apply from the last pointer position. Keep any
            // travel already spent inside a detent when a window rolls over.
            if (previous.snappedIndex !== undefined) combined.add(previous.snappedIndex);
            atTravel = timelineTimeDrag(previous.sampleIndex, scale, combined, previous.plateau, previous.phase);
            pickup = travel;
            next = atTravel(pixels - pickup);
        }
        travel = pixels;
        return previous = next;
    };
}
