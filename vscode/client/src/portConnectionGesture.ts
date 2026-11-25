import { canContinuePortConnection, canDetachConnection, canMoveConnectionEnd, connectionFromPorts, sameGraphPort, type GraphConnectionEnd, type GraphConnectionEdit, type GraphPortEndpoint } from "./graphConnectionEditing";
import type { GraphScope } from "./graphScope";
import { isTimelineSnapAddress } from "./timelineSnap";

export type PortPointerPosition = { x: number; y: number };
export type PortConnectionDrag = {
    pointerId: number;
    port: GraphPortEndpoint;
    start: PortPointerPosition;
    end: PortPointerPosition;
    moved: boolean;
    edit?: GraphConnectionEnd;
};
export type PortConnectionResult = { type: "create"; first: GraphPortEndpoint; second: GraphPortEndpoint; detach?: true }
    | { type: "edit"; edit: GraphConnectionEdit } | { type: "altClick" };

export function destinationHandle(hit: Element | null): HTMLElement | null {
    return hit?.closest<HTMLElement>("[data-port-endpoint]")
        ?? hit?.closest("[data-port-drop-target]")?.querySelector<HTMLElement>("[data-port-endpoint]") ?? null;
}

export function portConnectionDrop(hit: Element | null, surface: Element | null) {
    const encoded = destinationHandle(hit)?.dataset.portEndpoint;
    const target = encoded ? JSON.parse(encoded) as GraphPortEndpoint : null;
    // A port group can span the whole lane height. Its hover area is blank
    // graph space, even when it allows a continuation to open.
    const emptyEditorSpace = !target && !!hit && !!surface?.contains(hit)
        && !hit.closest(".port-panel, [data-port-control]:not(.port-column-hover), button, input, select, textarea, a, label, summary, [contenteditable], [role=button], [role=slider], [role=dialog], .package-dialog-backdrop");
    return { target, emptyEditorSpace };
}

export function beginPortConnection(port: GraphPortEndpoint, pointerId: number,
    button: number, position: PortPointerPosition, edit?: GraphConnectionEnd): PortConnectionDrag | null {
    return button === 0 ? { pointerId, port, start: position, end: position, moved: false, edit } : null;
}

function movedFromStart(drag: PortConnectionDrag, position: PortPointerPosition): boolean {
    return drag.moved || Math.hypot(position.x - drag.start.x, position.y - drag.start.y) >= 4;
}

export function movePortConnection(drag: PortConnectionDrag, pointerId: number,
    buttons: number, position: PortPointerPosition): PortConnectionDrag | null {
    if (pointerId !== drag.pointerId) return drag;
    if (!(buttons & 1)) return null;
    return { ...drag, end: position, moved: movedFromStart(drag, position) };
}

export function acceptsPortConnection(source: GraphPortEndpoint, target: GraphPortEndpoint, editing = false): boolean {
    return source.kind === target.kind && (editing ? source.direction === target.direction : source.direction !== target.direction);
}

// Shared by hover feedback and release. Converter availability is still checked
// by the backend; these are the editor's current connection eligibility checks.
export function canDropPortConnection(scope: GraphScope | null, drag: PortConnectionDrag,
    target: GraphPortEndpoint | null): boolean {
    if (!scope?.editable || !target || !acceptsPortConnection(drag.port, target, !!drag.edit)) return false;
    if (drag.edit) return !sameGraphPort(drag.port, target) && canMoveConnectionEnd(scope, drag.edit, target);
    if (isTimelineSnapAddress(drag.port.address) || isTimelineSnapAddress(target.address)) {
        try { connectionFromPorts(scope, "", drag.port, target); }
        catch { return false; }
    }
    return true;
}

export function finishPortConnection(scope: GraphScope | null, drag: PortConnectionDrag, pointerId: number,
    position: PortPointerPosition, target: GraphPortEndpoint | null, emptyEditorSpace = false,
    altHeld = false): PortConnectionResult | null {
    if (!scope?.editable || pointerId !== drag.pointerId) return null;
    if (!movedFromStart(drag, position)) return altHeld ? { type: "altClick" } : null;
    if (altHeld && target && (isTimelineSnapAddress(target.address) || isTimelineSnapAddress(drag.port.address)
        || (drag.edit && !canDetachConnection(drag.edit.connection.connection)))) return null;
    const detach = altHeld ? { detach: true as const } : {};
    if (drag.edit) {
        if (target && sameGraphPort(drag.port, target)) return null;
        if (target && canDropPortConnection(scope, drag, target)) return { type: "edit", edit: { end: drag.edit, target, ...detach } };
        if ((target || emptyEditorSpace) && canContinuePortConnection(scope, drag.port, drag.edit)) {
            return { type: "edit", edit: { end: drag.edit, target: null } };
        }
        return null;
    }
    if (!target || !canDropPortConnection(scope, drag, target)) return null;
    return { type: "create", first: drag.port, second: target, ...detach };
}
