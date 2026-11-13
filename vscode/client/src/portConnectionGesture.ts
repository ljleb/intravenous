import { sameGraphPort, type GraphConnectionEnd, type GraphConnectionEdit, type GraphPortEndpoint } from "./graphConnectionEditing";

export type PortPointerPosition = { x: number; y: number };
export type PortConnectionDrag = {
    pointerId: number;
    port: GraphPortEndpoint;
    start: PortPointerPosition;
    end: PortPointerPosition;
    moved: boolean;
    edit?: GraphConnectionEnd;
};
export type PortConnectionResult = { type: "create"; first: GraphPortEndpoint; second: GraphPortEndpoint }
    | { type: "edit"; edit: GraphConnectionEdit };

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

export function finishPortConnection(drag: PortConnectionDrag, pointerId: number,
    position: PortPointerPosition, target: GraphPortEndpoint | null, emptyEditorSpace = false): PortConnectionResult | null {
    if (pointerId !== drag.pointerId || !movedFromStart(drag, position)) return null;
    if (drag.edit) {
        if (target ? !acceptsPortConnection(drag.port, target, true) || sameGraphPort(drag.port, target) : !emptyEditorSpace) return null;
        return { type: "edit", edit: { end: drag.edit, target } };
    }
    if (!target || !acceptsPortConnection(drag.port, target)) return null;
    return { type: "create", first: drag.port, second: target };
}
