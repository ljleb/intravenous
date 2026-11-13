import type { ContinuousValueAlternative } from "./graphModel";

export type NodeAddress = { instanceId: string; path?: Array<Record<string, unknown>> };
export type GraphScopePort = {
    name: string;
    index: number;
    channelType?: string;
    eventType?: string;
    valueTypes?: ContinuousValueAlternative[];
};
export type GraphScopeNode = {
    address: NodeAddress;
    instance: {
        instanceId: string;
        definitionId: string;
        displayName?: string;
        realized: boolean;
        configurationSource?: string;
    };
    sampleInputs: GraphScopePort[];
    sampleOutputs: GraphScopePort[];
    eventInputs: GraphScopePort[];
    eventOutputs: GraphScopePort[];
    hasChildren: boolean;
};
export type GraphPortMatcher = NodeAddress & {
    port: { name?: string; index?: number; channel?: number };
};
export type GraphScopeConnection = {
    id: string;
    applied: boolean;
    connection: {
        kind: "sample" | "event";
        outputs: GraphPortMatcher[];
        inputs: GraphPortMatcher[];
    };
};
export type GraphScope = {
    generation: number | null;
    editable: boolean;
    nodes: GraphScopeNode[];
    connections: GraphScopeConnection[];
};
export type Point = { x: number; y: number };
export type GraphLayout = {
    positions: Record<string, Point>;
    viewport: Point & { scale: number };
};
export const nodeWidth = 300;
export const nodeHeight = 320;
export function nodeHeightFor(node: GraphScopeNode): number {
    return Math.max(nodeHeight, 100 + 24 * Math.max(
        node.sampleInputs.length + node.eventInputs.length,
        node.sampleOutputs.length + node.eventOutputs.length));
}

// NodeAddress has a fixed wire shape. Normalize the optional empty path so
// equivalent root addresses use the same persisted layout key.
export function addressKey(address: NodeAddress): string {
    return JSON.stringify([address.instanceId, address.path ?? []]);
}

export function readLayout(value: unknown): GraphLayout {
    const layout = value as Partial<GraphLayout> | null;
    const finitePoint = (point: unknown): point is Point => {
        if (!point || typeof point !== "object") return false;
        const candidate = point as Point;
        return Number.isFinite(candidate.x) && Number.isFinite(candidate.y);
    };
    const positions: Record<string, Point> = {};
    for (const [key, point] of Object.entries(layout?.positions ?? {})) {
        if (finitePoint(point)) positions[key] = { x: point.x, y: point.y };
    }
    const view = layout?.viewport;
    return {
        positions,
        viewport: finitePoint(view) && Number.isFinite(view.scale) && view.scale >= 0.1 && view.scale <= 2
            ? { x: view.x, y: view.y, scale: view.scale }
            : { x: 40, y: 40, scale: 1 },
    };
}

export function placeNodes(nodes: readonly GraphScopeNode[], saved: GraphLayout): GraphLayout {
    const positions = { ...saved.positions };
    // Retain removed nodes' positions: undo and temporary failed configuration
    // must not throw away the user's layout.
    const occupied = nodes.map((node) => positions[addressKey(node.address)]).filter(Boolean);
    const rowHeight = Math.max(nodeHeight, ...nodes.map(nodeHeightFor));
    let slot = 0;
    for (const node of nodes) {
        const key = addressKey(node.address);
        if (positions[key]) continue;
        let point: Point;
        do {
            point = { x: (slot % 4) * (nodeWidth + 60), y: Math.floor(slot / 4) * (rowHeight + 60) };
            ++slot;
        } while (occupied.some((other) => Math.abs(point.x - other.x) < nodeWidth + 20
            && Math.abs(point.y - other.y) < rowHeight + 20));
        positions[key] = point;
        occupied.push(point);
    }
    return { positions, viewport: { ...saved.viewport } };
}

export function zoomAt(viewport: GraphLayout["viewport"], cursor: Point, scale: number): GraphLayout["viewport"] {
    const next = Math.min(2, Math.max(0.1, scale));
    const ratio = next / viewport.scale;
    return { x: cursor.x - (cursor.x - viewport.x) * ratio,
        y: cursor.y - (cursor.y - viewport.y) * ratio, scale: next };
}

// A matcher into a module's interior cannot be attached to a guessed root
// port. Keep it in the connection list until that scope can be displayed.
export function resolveScopePorts(scope: GraphScope, matcher: GraphPortMatcher,
    kind: "sample" | "event", direction: "input" | "output") {
    if (matcher.path?.length) return [];
    const node = scope.nodes.find((entry) => entry.address.instanceId === matcher.instanceId);
    if (!node) return [];
    const ports = kind === "sample"
        ? direction === "input" ? node.sampleInputs : node.sampleOutputs
        : direction === "input" ? node.eventInputs : node.eventOutputs;
    const selected = ports.filter((entry) => (matcher.port.name === undefined || entry.name === matcher.port.name)
        && (matcher.port.index === undefined || entry.index === matcher.port.index));
    return selected.map((port) => ({ node, port, row: ports.indexOf(port) + (kind === "event"
        ? (direction === "input" ? node.sampleInputs.length : node.sampleOutputs.length) : 0) }));
}
