import { onBeforeUnmount, type Ref } from "vue";
import { automationSampleIndex } from "../../src/automationCurveGeometry";
import { timelineTimeDrag } from "../../src/timelineTimeDrag";
import { useOptionalGraphEditor } from "../graph/context";
import { visibleCursorDestination } from "../graph/cursorReturn";
import { webviewApi } from "../host";
import { snapTimelineSample, timelineSampleFraction, timelineSnapRadiusPx } from "./geometry";
import { usePlayback } from "./playbackContext";
import type { PresentationTimeWindow } from "./timelineContext";

export function usePlayheadSeek(surface: Ref<HTMLElement | null>, timeWindow: () => PresentationTimeWindow | undefined,
    marks: () => readonly bigint[]) {
    const editor = useOptionalGraphEditor();
    const { backgroundPlayhead, dragPosition, releaseDrag, state } = usePlayback();
    let cancel: (() => void) | undefined;
    onBeforeUnmount(() => cancel?.());

    return function beginSeek(event: PointerEvent) {
        const element = surface.value, time = timeWindow();
        if (event.button !== 0 || !editor || !element || !time || cancel) return;
        const trace = editor.takePlayheadTrace();
        const bounds = element.getBoundingClientRect();
        const span = time.spanSeconds ?? time.endSeconds - time.beginSeconds;
        trace?.log("Playhead press", { surface: element.classList.contains("time-ruler") ? "ruler" : "lane",
            pointerType: event.pointerType, pointerId: event.pointerId, buttons: event.buttons,
            client: { x: event.clientX, y: event.clientY }, screen: { x: event.screenX, y: event.screenY },
            scale: window.devicePixelRatio, beginSeconds: time.beginSeconds, spanSeconds: span, width: bounds.width });
        if (!(bounds.width > 0 && span > 0)) return;
        const sampleRate = state.value?.sampleRate ?? time.sampleRate ?? 48000;
        const secondsPerPixel = span / bounds.width;
        const pressX = event.clientX, pressY = event.clientY;
        const anchor = automationSampleIndex(time.beginSeconds, sampleRate, (pressX - bounds.left) * secondsPerPixel);
        if (anchor === undefined) return;
        const snapTargets = marks();
        trace?.log("Seek mapping", { anchor: anchor.toString(), sampleRate, secondsPerPixel, detents: snapTargets.length,
            currentPlayhead: backgroundPlayhead.value?.toString(), running: state.value?.running });
        const atTravel = timelineTimeDrag(anchor, secondsPerPixel, sampleRate, snapTargets);
        let travel = 0;
        let position: string | null = null;
        let posted: string | null = null;
        let frame = 0;

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
            } else if (!frame) frame = requestAnimationFrame(() => {
                frame = 0;
                if (position === null || position === posted) return;
                posted = position;
                webviewApi.postMessage({ type: "playbackDrag", phase: "move", position });
            });
        };
        cancel = editor.startObjectDrag(event, {
            trace,
            move: (point) => update(point.x - pressX),
            moveBy: (delta) => update(travel + delta.x),
            finish(point, lockOrigin) {
                cancel = undefined;
                if (point && !lockOrigin) update(point.x - pressX);
                cancelAnimationFrame(frame);
                const finalPosition = position;
                trace?.log("Seek gesture ended", { finalPosition, travel, release: point, lockOrigin });
                if (finalPosition === null) {
                    if (point) {
                        const index = BigInt.asIntN(64, anchor);
                        const snapped = snapTimelineSample(index, snapTargets, timelineSnapRadiusPx * secondsPerPixel * sampleRate);
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
                    const fraction = timelineSampleFraction(displayed, currentTime, state.value?.sampleRate ?? sampleRate);
                    const requested = { x: rect.left + fraction * rect.width, y: pressY };
                    const destination = visibleCursorDestination(element, requested);
                    trace?.log("Resolved playhead destination", { displayed: displayed.toString(),
                        applied: state.value?.position, requested, destination });
                    return destination;
                } };
            },
        });
    };
}
