import { onBeforeUnmount, type Ref } from "vue";
import { playheadTimeDrag } from "../../src/timelineTimeDrag";
import { useOptionalGraphEditor } from "../graph/context";
import { visibleCursorDestination } from "../graph/cursorReturn";
import { webviewApi } from "../host";
import { seekBeatWindow, snapTimelineSample, timelineSampleFraction, timelineSnapRadiusPx, type SeekBeatWindow } from "../../src/timelineGeometry";
import { projectSampleRateFallback, timelineObservationSpan, timelineSampleAt, timelineSamples } from "../../src/timelineSamples";
import { usePlayback } from "./playbackContext";
import type { PresentationTimeWindow } from "./timelineContext";
import { usePresentationFrame } from "../presentation/presentationFrames";

export type SeekBeatReader = {
    marks: (window: SeekBeatWindow, sampleRate: number, width: number) => readonly bigint[] | undefined;
    close?: () => void;
};

export function usePlayheadSeek(surface: Ref<HTMLElement | null>, timeWindow: () => PresentationTimeWindow | undefined,
    marks: () => readonly bigint[], openBeatReader?: () => SeekBeatReader | undefined) {
    const editor = useOptionalGraphEditor();
    const { backgroundPlayhead, dragPosition, releaseDrag, state } = usePlayback();
    let cancel: (() => void) | undefined;
    let closeReader: (() => void) | undefined;
    let publishMove: (() => void) | undefined;
    const frame = usePresentationFrame("update", () => publishMove?.());
    onBeforeUnmount(() => { cancel?.(); closeReader?.(); });

    return function beginSeek(event: PointerEvent) {
        const element = surface.value, time = timeWindow();
        if (event.button !== 0 || !editor || !element || !time || cancel) return;
        const trace = editor.takeCursorReturnTrace();
        const bounds = element.getBoundingClientRect();
        const view = timelineSamples(time);
        const sampleRate = time.sampleRate ?? state.value?.sampleRate ?? projectSampleRateFallback;
        const span = view.span / sampleRate;
        trace?.log("Playhead press", { surface: element.classList.contains("time-ruler") ? "ruler" : "lane",
            pointerType: event.pointerType, pointerId: event.pointerId, buttons: event.buttons,
            client: { x: event.clientX, y: event.clientY }, screen: { x: event.screenX, y: event.screenY },
            scale: window.devicePixelRatio, beginSample: time.beginSample, spanSeconds: span, width: bounds.width });
        if (!(bounds.width > 0 && span > 0)) return;
        const secondsPerPixel = span / bounds.width;
        const samplesPerPixel = view.span / bounds.width;
        const pressX = event.clientX, pressY = event.clientY;
        const anchor = timelineSampleAt(view, (pressX - bounds.left) / bounds.width);
        const snapTargets = marks();
        trace?.log("Seek mapping", { anchor: anchor.toString(), sampleRate, secondsPerPixel, detents: snapTargets.length,
            currentPlayhead: backgroundPlayhead.value?.toString(), running: state.value?.running });
        const { begin, length } = timelineObservationSpan(view);
        const reader = openBeatReader?.();
        const visible = { first: BigInt(begin), length: BigInt(length) };
        closeReader = reader?.close;
        let readTargets: readonly bigint[] | undefined;
        let outsideTargets: readonly bigint[] = [];
        const atTravel = playheadTimeDrag(anchor, { spanSamples: view.span, width: bounds.width }, snapTargets, reader ? (sample, forward) => {
            const window = seekBeatWindow(visible, sample, forward);
            if (!window) return;
            const width = Number(window.length) / samplesPerPixel;
            const targets = reader.marks(window, sampleRate, width);
            if (!targets) return;
            if (targets !== readTargets) {
                readTargets = targets;
                outsideTargets = targets.filter((target) => BigInt.asUintN(64, target - visible.first) >= visible.length);
            }
            return outsideTargets;
        } : undefined);
        let travel = 0;
        let position: string | null = null;
        let posted: string | null = null;
        publishMove = () => {
            if (position === null || position === posted) return;
            posted = position;
            webviewApi.postMessage({ type: "playbackDrag", phase: "move", position });
        };

        const update = (pixels: number) => {
            if (pixels === travel) return;
            travel = pixels;
            const next = atTravel(travel).sampleIndex.toString();
            if (next === position) return;
            const began = position !== null;
            position = dragPosition.value = next;
            if (!began) {
                posted = next;
                webviewApi.postMessage({ type: "playbackDrag", phase: "begin", position: next });
            } else frame.schedule();
        };
        cancel = editor.startObjectDrag(event, {
            trace,
            move: (point) => update(point.x - pressX),
            moveBy: (delta) => update(travel + delta.x),
            finish(point, lockOrigin) {
                cancel = undefined;
                if (point && !lockOrigin) update(point.x - pressX);
                closeReader?.();
                closeReader = undefined;
                frame.cancel();
                publishMove = undefined;
                const finalPosition = position;
                trace?.log("Seek gesture ended", { finalPosition, travel, release: point, lockOrigin });
                if (finalPosition === null) {
                    if (point) {
                        const index = BigInt.asIntN(64, anchor);
                        const snapped = snapTimelineSample(index, snapTargets, timelineSnapRadiusPx * samplesPerPixel);
                        webviewApi.postMessage({ type: "playbackSeek", position: snapped.toString() });
                    }
                    return;
                }
                const releaseId = releaseDrag(finalPosition);
                webviewApi.postMessage({ type: "playbackDrag", phase: "end", position: finalPosition, releaseId });
                return { policy: "playhead", destination: () => {
                    const currentTime = timeWindow();
                    const displayed = backgroundPlayhead.value;
                    if (!currentTime || !element.isConnected || displayed === null) {
                        trace?.log("Playhead has no destination", { connected: element.isConnected,
                            hasTimeWindow: !!currentTime, displayed: displayed?.toString() });
                        return null;
                    }
                    const rect = element.getBoundingClientRect();
                    const fraction = timelineSampleFraction(displayed, currentTime);
                    const requested = { x: rect.left + fraction * rect.width, y: pressY };
                    const destination = visibleCursorDestination(element, requested);
                    trace?.log("Resolved playhead destination", { displayed: displayed.toString(),
                        applied: state.value?.position, requested, destination });
                    return destination;
                } };
            },
        });
        if (!cancel) { closeReader?.(); closeReader = undefined; publishMove = undefined; }
    };
}
