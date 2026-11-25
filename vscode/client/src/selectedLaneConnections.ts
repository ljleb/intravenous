import { addressKey, matchingScopePorts, type GraphScope, type NodeAddress, type Point } from "./graphScope";
import type { GraphPortEndpoint } from "./graphConnectionEditing";

export type PortWireAnchor = { id: string; x: number; y: number; moduleBoundary?: boolean; rulerInput?: boolean };
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
    return JSON.stringify([addressKey(port.address), port.direction, port.kind, port.index, port.name, !!port.moduleBoundary]);
}

export function portSlotKey(address: NodeAddress, direction: "input" | "output", moduleBoundary = false): string {
    return JSON.stringify([addressKey(address), direction, moduleBoundary]);
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
    return scope.connections.map((entry) => {
        const { connection } = entry;
        const resolve = (direction: "input" | "output") => {
            const matchers = direction === "input" ? connection.inputs : connection.outputs;
            const ports = new Map<string, { port: GraphPortEndpoint; key: string; address: string }>();
            for (const matcher of matchers) {
                const matcherKey = addressKey(matcher);
                const node = nodes.get(matcherKey) ?? nodes.get(aliases.get(matcherKey) ?? "");
                if (!node) continue;
                const address = addressKey(node.address);
                for (const { port: matched } of matchingScopePorts(node, matcher, connection.kind, direction)) {
                    const port: GraphPortEndpoint = { address: node.address, kind: connection.kind,
                        direction, index: matched.index, name: matched.name, moduleBoundary: matcher.moduleBoundary };
                    const key = portEndpointKey(port);
                    ports.set(key, { port, key, address });
                }
            }
            return [...ports.values()];
        };
        const hiddenSampleSources = (entry.hiddenSampleSourceModules ?? []).map(addressKey)
            .filter((address) => nodes.has(address));
        return { id: entry.id, applied: entry.applied, sourceOwned: !!entry.sourceOwned,
            outputs: resolve("output"), inputs: resolve("input"), hiddenSampleSources };
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
            if (!connection.inputs.length || (!connection.outputs.length && !connection.hiddenSampleSources.length)) continue;
            let first = Infinity;
            let last = -Infinity;
            const include = (address: string) => {
                const row = rows.get(address);
                // A ruler socket is outside the flow of lane rows.
                first = Math.min(first, row?.first ?? -Infinity);
                last = Math.max(last, row?.last ?? Infinity);
            };
            for (const { address } of connection.inputs) include(address);
            for (const { address } of connection.outputs) include(address);
            for (const address of connection.hiddenSampleSources) include(address);
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
    anchorFor: (port: GraphPortEndpoint, connectionId: string) =>
        PortWireAnchor | readonly PortWireAnchor[] | null,
    revealedEnds: readonly RevealedConnectionEnd[] = [],
    context?: { top: number; bottom: number; hiddenSourceAnchors?: ReadonlyMap<string, readonly PortWireAnchor[]> }): SelectedLaneWire[] {
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
            for (const { port, key, address } of ports) {
                const anchors = anchorFor(port, entry.id);
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
        for (const address of entry.hiddenSampleSources) {
            for (const anchor of context?.hiddenSourceAnchors?.get(address) ?? [])
                outputs.push({ anchor, selected: selected.has(address), revealed: false });
        }
        if (!outputs.length) continue;
        const inputs = resolve(entry.inputs);
        for (const output of outputs) {
            for (const input of inputs) {
                const source = output.anchor;
                const target = input.anchor;
                const emphasized = output.selected || input.selected;
                const sourceOwned = entry.sourceOwned;
                // The sticky ruler accepts cables from the lanes beneath it.
                if (context && target.rulerInput && source.y < context.top) continue;
                // The curve stays within the vertical span of its ports.
                const inViewport = context && (target.rulerInput || (Math.max(source.y, target.y) >= context.top
                    && Math.min(source.y, target.y) <= context.bottom));
                const requested = emphasized || output.revealed || input.revealed;
                if (!inViewport && (!requested || (context && sourceOwned))) continue;
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
    if (wire.sourceOwned && !!source.moduleBoundary !== !!target.moduleBoundary
        && Math.abs(source.x - target.x) < 12) {
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
