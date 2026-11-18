import { onBeforeUnmount, onMounted, shallowRef } from "vue";
import type { HoverElement } from "../src/trustedHover";
import type { AutomationPointPosition } from "../src/automationCurveGeometry";
import type { StatusHint } from "../src/statusBarText";

const hintSelector = '[data-status-hint]:not([data-status-hint=""]), [data-status-shortcuts]:not([data-status-shortcuts=""])';
const hintTarget = (target: Element | null) => target?.closest(hintSelector) ?? null;
function elementHint(target: Element | null): StatusHint | null {
    return target ? { text: target.getAttribute("data-status-hint") || undefined,
        shortcuts: target.getAttribute("data-status-shortcuts") || undefined } : null;
}

export function useElementStatusHint() {
    const hint = shallowRef<StatusHint | null>(null);
    let hovered: Element | null = null;
    let preferFocus = false;

    function refresh() {
        const focused = document.hasFocus() ? elementHint(hintTarget(document.activeElement)) : null;
        const hoveredHint = hovered?.isConnected ? elementHint(hovered) : null;
        const next = document.hidden ? null : preferFocus ? focused ?? hoveredHint : hoveredHint ?? focused;
        if (next?.text !== hint.value?.text || next?.shortcuts !== hint.value?.shortcuts) hint.value = next;
    }
    function focus(event: FocusEvent) {
        preferFocus = event.target instanceof Element && event.target.matches(":focus-visible");
        refresh();
    }
    function blur() {
        hovered = null;
        refresh();
    }
    function focusOut() { queueMicrotask(refresh); }
    onMounted(() => {
        document.addEventListener("focusin", focus);
        document.addEventListener("focusout", focusOut);
        document.addEventListener("visibilitychange", blur);
        window.addEventListener("focus", refresh);
        window.addEventListener("blur", blur);
        refresh();
    });
    onBeforeUnmount(() => {
        document.removeEventListener("focusin", focus);
        document.removeEventListener("focusout", focusOut);
        document.removeEventListener("visibilitychange", blur);
        window.removeEventListener("focus", refresh);
        window.removeEventListener("blur", blur);
    });
    return {
        hint,
        hover(target: HoverElement | null, pointerMoved: boolean) {
            const next = hintTarget(target instanceof Element ? target : null);
            if (pointerMoved || next !== hovered) preferFocus = false;
            hovered = next;
            refresh();
        },
    };
}

export function automationPointStatus(point: AutomationPointPosition): string {
    const time = point.sampleIndex !== undefined ? `${point.sampleIndex} smp` : `${Number(point.seconds.toPrecision(6))} s`;
    return `${time} · Value ${Number(point.value.toPrecision(6))}`
        + (point.tension === undefined ? "" : ` · Tension ${Number(point.tension.toFixed(2))}`);
}
