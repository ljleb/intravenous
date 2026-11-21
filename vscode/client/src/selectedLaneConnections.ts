import { addressKey, matchingScopePorts, type GraphPortMatcher, type GraphScope, type NodeAddress, type Point } from "./graphScope";
import type { GraphPortEndpoint } from "./graphConnectionEditing";

export type PortWireAnchor = { id: string; x: number; y: number; moduleBoundary?: boolean };
export type RevealedConnectionEnd = { connectionId: string; port: GraphPortEndpoint };
export type SelectedLaneWire = {
    key: string;
    source: PortWireAnchor;
    target: PortWireAnchor;
    connectionIds: string[];
    applied: boolean;
    emphasized: boolean;
    sourceOwned: boolean;
};

export function portEndpointKey(port: GraphPortEndpoint): string {
    return JSON.stringify([addressKey(port.address), port.direction, port.kind, port.index, port.name]);
}

export function portSlotKey(address: NodeAddress, direction: "input" | "output"): string {
    return JSON.stringify([addressKey(address), direction]);
}

export function connectionEndKey(connectionId: string, port: GraphPortEndpoint): string {
    return JSON.stringify([connectionId, portEndpointKey(port)]);
}

export function resolveLaneConnectionEnds(scope: GraphScope) {
    const nodes = new Map(scope.nodes.map((node) => [addressKey(node.address), node]));
    const aliases = new Map<string, string>();
    for (const child of scope.childScopes ?? []) {
        for (const node of child.nodes) {
            if (node.membershipAddress) aliases.set(addressKey(node.membershipAddress), addressKey(node.address));
        }
    }
    const moduleBoundaries = new Set(scope.childScopes?.filter((child) => child.hasBoundary).map(({ subject }) => addressKey(subject)));
    return scope.connections.map((entry) => {
        const { connection } = entry;
        const resolve = (direction: "input" | "output") => {
            const matchers = direction === "input" ? connection.inputs : connection.outputs;
            const peers = direction === "input" ? connection.outputs : connection.inputs;
            const boundary = direction === "input" ? entry.boundaryTargets : entry.boundarySources;
            const opposite = direction === "input" ? "output" : "input";
            const ports = new Map<string, { port: GraphPortEndpoint; key: string; address: string; moduleBoundary: boolean }>();
            const add = (endpoints: readonly GraphPortMatcher[], portDirection: typeof direction, moduleBoundary: boolean) => {
                for (const matcher of endpoints) {
                    const matcherKey = addressKey(matcher);
                    const node = nodes.get(matcherKey) ?? nodes.get(aliases.get(matcherKey) ?? "");
                    if (!node) continue;
                    const address = addressKey(node.address);
                    const pathLength = node.address.path?.length ?? 0;
                    const interior = !entry.sourceOwned && peers.length > 0 && moduleBoundaries.has(address)
                        && peers.every((peer) => (peer.path?.length ?? 0) > pathLength
                            && addressKey({ instanceId: peer.instanceId, path: peer.path?.slice(0, pathLength) }) === address);
                    for (const resolved of matchingScopePorts(node, matcher, connection.kind, portDirection)) {
                        const port: GraphPortEndpoint = { address: node.address, kind: connection.kind,
                            direction: portDirection, index: resolved.port.index, name: resolved.port.name };
                        const key = portEndpointKey(port);
                        ports.set(key, { port, key, address, moduleBoundary: moduleBoundary || interior });
                    }
                }
            };
            add(matchers, direction, false);
            if (boundary) add(boundary, opposite, true);
            return [...ports.values()];
        };
        return { id: entry.id, applied: entry.applied, sourceOwned: !!entry.sourceOwned,
            outputs: resolve("output"), inputs: resolve("input") };
    });
}

export type ResolvedLaneConnections = ReturnType<typeof resolveLaneConnectionEnds>;
type ResolvedLaneConnection = ResolvedLaneConnections[number];
type ConnectionLaneSpan = { first: number; last: number; connection: ResolvedLaneConnection };

export class LaneConnectionViewport {
    private readonly starts: ConnectionLaneSpan[] = [];
    private readonly ends: ConnectionLaneSpan[];
    private readonly visible = new Set<ResolvedLaneConnection>();
    private nextStart = 0;
    private nextEnd = 0;

    constructor(connections: ResolvedLaneConnections, rowAddresses: readonly string[]) {
        const rows = new Map<string, { first: number; last: number }>();
        rowAddresses.forEach((address, index) => {
            const span = rows.get(address);
            if (span) span.last = index;
            else rows.set(address, { first: index, last: index });
        });
        for (const connection of connections) {
            if (!connection.inputs.length || !connection.outputs.length) continue;
            let first = Infinity;
            let last = -Infinity;
            for (const { address } of [...connection.inputs, ...connection.outputs]) {
                const row = rows.get(address);
                // A ruler socket is outside the flow of lane rows.
                first = Math.min(first, row?.first ?? -Infinity);
                last = Math.max(last, row?.last ?? Infinity);
            }
            if (Number.isFinite(first)) this.starts.push({ first, last, connection });
            else this.visible.add(connection);
        }
        this.starts.sort((left, right) => left.first - right.first);
        this.ends = this.starts.slice().sort((left, right) => left.last - right.last);
    }

    inRows(first: number, last: number): ReadonlySet<ResolvedLaneConnection> {
        const update = ({ first: start, last: end, connection }: ConnectionLaneSpan) => {
            if (start <= last && end >= first) this.visible.add(connection);
            else this.visible.delete(connection);
        };
        // Sweep all crossed boundaries in either direction. Each changed
        // span is evaluated against the final viewport, including on jumps.
        while (this.nextStart > 0 && this.starts[this.nextStart - 1].first > last)
            update(this.starts[--this.nextStart]);
        while (this.nextStart < this.starts.length && this.starts[this.nextStart].first <= last)
            update(this.starts[this.nextStart++]);
        while (this.nextEnd > 0 && this.ends[this.nextEnd - 1].last >= first)
            update(this.ends[--this.nextEnd]);
        while (this.nextEnd < this.ends.length && this.ends[this.nextEnd].last < first)
            update(this.ends[this.nextEnd++]);
        return this.visible;
    }
}

// A collapsed slot can represent several logical ports. Merge only the
// drawing's visible endpoint pairs; preserve the underlying connection IDs.
export function selectedLaneConnections(connections: Iterable<ResolvedLaneConnection>, selected: ReadonlySet<string>,
    anchorFor: (port: GraphPortEndpoint, connectionId: string, moduleBoundary?: boolean) =>
        PortWireAnchor | readonly PortWireAnchor[] | null,
    revealedEnds: readonly RevealedConnectionEnd[] = [],
    context?: { top: number; bottom: number; headerTarget?: { left: number; right: number; top: number; bottom: number } }): SelectedLaneWire[] {
    if (!selected.size && !revealedEnds.length && !context) return [];
    const revealed = new Map<string, Set<string>>();
    for (const end of revealedEnds) {
        const ports = revealed.get(end.connectionId) ?? new Set<string>();
        ports.add(portEndpointKey(end.port));
        revealed.set(end.connectionId, ports);
    }
    const wires = new Map<string, SelectedLaneWire>();
    for (const entry of connections) {
        if (!context && !revealed.has(entry.id) && !entry.outputs.some(({ address }) => selected.has(address))
            && !entry.inputs.some(({ address }) => selected.has(address))) continue;
        const resolve = (ports: typeof entry.outputs) => {
            const visible = new Map<string, { anchor: PortWireAnchor; selected: boolean; revealed: boolean }>();
            for (const { port, key, address, moduleBoundary } of ports) {
                const anchors = anchorFor(port, entry.id, moduleBoundary);
                if (!anchors) continue;
                const isSelected = selected.has(address);
                const isRevealed = revealed.get(entry.id)?.has(key) ?? false;
                for (const anchor of "id" in anchors ? [anchors] : anchors) {
                    const existing = visible.get(anchor.id);
                    if (existing) {
                        existing.selected ||= isSelected;
                        existing.revealed ||= isRevealed;
                    } else visible.set(anchor.id, { anchor, selected: isSelected, revealed: isRevealed });
                }
            }
            return [...visible.values()];
        };
        const outputs = resolve(entry.outputs);
        if (!outputs.length) continue;
        const inputs = resolve(entry.inputs);
        for (const output of outputs) {
            for (const input of inputs) {
                const source = output.anchor;
                const target = input.anchor;
                const emphasized = output.selected || input.selected;
                const header = context?.headerTarget;
                const inHeader = header && target.x >= header.left && target.x <= header.right && target.y >= header.top && target.y <= header.bottom;
                const sourceOwned = entry.sourceOwned;
                if (sourceOwned && context) {
                    // The curve stays within the vertical span of its ports.
                    // Even two offscreen ports can have a cable in view.
                    if (!inHeader && (Math.max(source.y, target.y) < context.top
                        || Math.min(source.y, target.y) > context.bottom)) continue;
                } else if (!emphasized && !output.revealed && !input.revealed
                    && (!context || source.y < context.top || source.y > context.bottom
                        || (!inHeader && (target.y < context.top || target.y > context.bottom)))) continue;
                const key = JSON.stringify([source.id, target.id, sourceOwned]);
                const existing = wires.get(key);
                if (existing) {
                    if (!existing.connectionIds.includes(entry.id)) existing.connectionIds.push(entry.id);
                    existing.applied = existing.applied && entry.applied;
                    existing.emphasized ||= emphasized;
                } else {
                    wires.set(key, { key, source, target, connectionIds: [entry.id], applied: entry.applied, emphasized, sourceOwned });
                }
            }
        }
    }
    return [...wires.values()];
}

export function selectedLaneWirePath(wire: SelectedLaneWire): string {
    const { source, target } = wire;
    if (wire.sourceOwned && !!source.moduleBoundary !== !!target.moduleBoundary) {
        // Boundary and child ports can lie in the same column. Bend into the
        // lane so their cable stays visible beside the overlapping headers.
        const bend = Math.max(source.x, target.x) + Math.max(22, Math.min(88, Math.abs(source.y - target.y) / 3));
        return `M ${source.x} ${source.y} C ${bend} ${source.y}, ${bend} ${target.y}, ${target.x} ${target.y}`;
    }
    return connectionWirePath(wire.source, wire.target);
}

export function connectionWirePath(first: Point, second: Point): string {
    const middleX = (first.x + second.x) / 2;
    return `M ${first.x} ${first.y} C ${middleX} ${first.y}, ${middleX} ${second.y}, ${second.x} ${second.y}`;
}
