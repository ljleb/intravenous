import type { ManualControlDescriptor } from "./manualControls";
import type { ContinuousValueAlternative } from "./graphModel";
import type { SerializedPresentationState } from "./presentationStateJson";
import { readTimelineViewport, type TimelineViewport } from "./timelineViewport";

export type NodeAddress = { instanceId: string; path?: Array<Record<string, unknown>> };
export type PortShape = "sequential" | "region" | "coverage";
export type PortRealization = "buffered" | "computed";
export type GraphScopePort = {
    name: string;
    index: number;
    displayName?: string;
    channelType?: string;
    eventType?: string;
    valueTypes?: ContinuousValueAlternative[];
    manualControl?: ManualControlDescriptor;
    access?: PortShape;
    production?: PortShape;
    realization?: PortRealization;
};
export type GraphScopeNode = {
    loadState: "waitingForDefinition" | "waitingForGraph" | "ready" | "failed";
    address: NodeAddress;
    membershipAddress?: NodeAddress;
    symbolicNode?: boolean;
    symbolicMemberCount?: number;
    childDisplayName?: string;
    childSourceLabel?: string;
    declarationSource?: { filePath: string; line: number; byteOffset: number };
    // Accepted definition when realized; instance.definitionId is the desired
    // declaration and can differ after a durable node replacement.
    definitionId?: string;
    instance: {
        instanceId: string;
        definitionId: string;
        displayName?: string;
        defaultDisplayName?: string;
        realized: boolean;
        configurationSource?: string;
        packageRoot?: string;
    };
    sampleInputs: GraphScopePort[];
    sampleOutputs: GraphScopePort[];
    eventInputs: GraphScopePort[];
    eventOutputs: GraphScopePort[];
    hasChildren: boolean;
    isModule?: boolean;
    errorMessage?: string;
    // Accepted constructor values, independent of desired configurationSource.
    construction?: { definitionId: string; arguments: unknown[] } | null;
    constructionError?: string;
};

export function laneLabels(displayName: string, sourceLabel?: string) {
    return sourceLabel
        ? { primary: sourceLabel, secondary: displayName }
        : { primary: displayName, secondary: undefined };
}

export function formatLaneLabel({ primary, secondary }: ReturnType<typeof laneLabels>): string {
    return secondary ? `${primary} · ${secondary}` : primary;
}

export function graphScopeNodeLabel(node: GraphScopeNode): string {
    const name = node.childDisplayName || node.instance.displayName || node.instance.defaultDisplayName
        || node.instance.definitionId.split(".").pop() || node.instance.definitionId;
    return formatLaneLabel(laneLabels(name, node.childSourceLabel));
}

export type GraphPortMatcher = NodeAddress & {
    moduleBoundary?: boolean;
    port: { name?: string; index?: number; channel?: number };
};
export type GraphScopeConnection = {
    id: string;
    applied: boolean;
    sourceOwned?: boolean;
    connection: {
        connectionId?: string;
        kind: "sample" | "event";
        sourceType?: string;
        targetType?: string;
        outputs: GraphPortMatcher[];
        inputs: GraphPortMatcher[];
    };
};
export type GraphScope = {
    sessionRevision?: number;
    sampleRate?: number | null;
    execution?: { phase: string; message: string; preparedGeneration: number | null; activeGeneration: number | null };
    generation: number | null;
    editable: boolean;
    nodes: GraphScopeNode[];
    childScopes?: Array<{ subject: NodeAddress; nodes: GraphScopeNode[]; hasBoundary?: boolean }>;
    connections: GraphScopeConnection[];
    timelineNodes?: NodeAddress[];
    unavailableLaneParents?: NodeAddress[];
    presentation?: ProjectPresentation;
};
export type Point = { x: number; y: number };
export type GraphLaneCopy = { sourceInstanceId: string; instanceId: string };
export type ProjectLaneView = {
    address: NodeAddress;
    presentationId: string;
    version: string;
};
export const laneViewReadBatchSize = 8;
export type ProjectEditReceipt = { sessionRevision: number; laneViews?: ProjectLaneView[] };
export type ProjectPresentation = {
    laneOrder: NodeAddress[];
    laneParents?: Array<{ instanceId: string; parent: NodeAddress }>;
    laneViews: ProjectLaneView[];
    timelineSnapMemory: import("./timelineSnap").TimelineSnapMemory;
};
export type GraphViewState = {
    laneOrder: string[];
    laneParents: Record<string, string>;
};
export type GraphLayout = GraphViewState & {
    positions: Record<string, Point>;
    viewport: Point & { scale: number };
    timeline: TimelineViewport;
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

export function isProjectInstance(address: NodeAddress): boolean {
    return !address.path?.length;
}

export function graphScopeWithChildren(scope: GraphScope): GraphScope {
    if (!scope.childScopes?.length) return scope;
    const unique = new Map(scope.nodes.map((node) => [addressKey(node.address), node]));
    for (const { nodes } of scope.childScopes)
        for (const node of nodes) unique.set(addressKey(node.address), node);
    return { ...scope, nodes: [...unique.values()] };
}

export function presentationStateKey(address: NodeAddress, presentationId: string): string {
    return JSON.stringify([presentationId, address.instanceId, address.path ?? []]);
}

export function addressFromKey(key: string): NodeAddress {
    const [instanceId, path] = JSON.parse(key) as [string, NodeAddress["path"]];
    return { instanceId, path };
}

export function laneViewFromKey(key: string, state: SerializedPresentationState): Omit<ProjectLaneView, "version"> & { state: SerializedPresentationState } {
    const [presentationId, instanceId, path] = JSON.parse(key) as [string, string, NodeAddress["path"]];
    return { address: { instanceId, path }, presentationId, state };
}

export function readGraphViewState(presentation?: ProjectPresentation): GraphViewState {
    return {
        laneOrder: presentation?.laneOrder.map(addressKey) ?? [],
        laneParents: Object.fromEntries((presentation?.laneParents ?? []).map(({ instanceId, parent }) =>
            [addressKey({ instanceId }), addressKey(parent)])),
    };
}

export function newGraphLayout(): GraphLayout {
    return {
        positions: {},
        ...readGraphViewState(),
        timeline: readTimelineViewport(undefined),
        viewport: { x: 40, y: 40, scale: 1 },
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
    return { ...saved, positions, viewport: { ...saved.viewport }, timeline: { ...saved.timeline } };
}

export function zoomAt(viewport: GraphLayout["viewport"], cursor: Point, scale: number): GraphLayout["viewport"] {
    const next = Math.min(2, Math.max(0.1, scale));
    const ratio = next / viewport.scale;
    return { x: cursor.x - (cursor.x - viewport.x) * ratio,
        y: cursor.y - (cursor.y - viewport.y) * ratio, scale: next };
}

export function scopeNodeForAddress(scope: GraphScope, address: NodeAddress): GraphScopeNode | undefined {
    const key = addressKey(address);
    const exact = scope.nodes.find((entry) => addressKey(entry.address) === key);
    if (exact) return exact;
    for (const childScope of scope.childScopes ?? []) {
        const member = childScope.nodes.find((entry) => entry.membershipAddress && addressKey(entry.membershipAddress) === key);
        if (member) return scope.nodes.find((entry) => addressKey(entry.address) === addressKey(member.address));
    }
}

export function resolveScopePorts(scope: GraphScope, matcher: GraphPortMatcher,
    kind: "sample" | "event", direction: "input" | "output") {
    const node = scopeNodeForAddress(scope, matcher);
    return node ? matchingScopePorts(node, matcher, kind, direction) : [];
}

export function matchingScopePorts(node: GraphScopeNode, matcher: GraphPortMatcher,
    kind: "sample" | "event", direction: "input" | "output") {
    const publicDirection = matcher.moduleBoundary ? oppositePortDirection(direction) : direction;
    const ports = kind === "sample"
        ? publicDirection === "input" ? node.sampleInputs : node.sampleOutputs
        : publicDirection === "input" ? node.eventInputs : node.eventOutputs;
    const selected = ports.filter((entry) => (matcher.port.name === undefined || entry.name === matcher.port.name)
        && (matcher.port.index === undefined || entry.index === matcher.port.index));
    return selected.map((port) => ({ node, port, row: ports.indexOf(port) + (kind === "event"
        ? (publicDirection === "input" ? node.sampleInputs.length : node.sampleOutputs.length) : 0) }));
}

export function oppositePortDirection(direction: "input" | "output"): "input" | "output" {
    return direction === "input" ? "output" : "input";
}
