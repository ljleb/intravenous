import { addressKey, resolveScopePorts, type GraphScope, type NodeAddress, type Point } from "./graphScope";
import type { GraphPortEndpoint } from "./graphConnectionEditing";

export type PortWireAnchor = { id: string; x: number; y: number };
export type RevealedConnectionEnd = { connectionId: string; port: GraphPortEndpoint };
export type SelectedLaneWire = {
    key: string;
    source: PortWireAnchor;
    target: PortWireAnchor;
    connectionIds: string[];
    applied: boolean;
    emphasized: boolean;
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

// A collapsed slot can represent several logical ports. Merge only the
// drawing's visible endpoint pairs; preserve the underlying connection IDs.
export function selectedLaneConnections(scope: GraphScope, selected: ReadonlySet<string>,
    anchorFor: (port: GraphPortEndpoint, connectionId: string) => PortWireAnchor | null,
    revealedEnds: readonly RevealedConnectionEnd[] = [],
    context?: { top: number; bottom: number }): SelectedLaneWire[] {
    const selectedInstances = new Set(scope.nodes.filter((node) => selected.has(addressKey(node.address)))
        .map((node) => node.instance.instanceId));
    if (!selectedInstances.size && !revealedEnds.length && !context) return [];
    const revealed = new Map<string, Set<string>>();
    for (const end of revealedEnds) {
        const ports = revealed.get(end.connectionId) ?? new Set<string>();
        ports.add(portEndpointKey(end.port));
        revealed.set(end.connectionId, ports);
    }
    const wires = new Map<string, SelectedLaneWire>();
    for (const entry of scope.connections) {
        const { connection } = entry;
        if (!context && !revealed.has(entry.id) && ![...connection.outputs, ...connection.inputs]
            .some((matcher) => selectedInstances.has(matcher.instanceId))) continue;
        const resolve = (direction: "input" | "output") => {
            const matchers = direction === "input" ? connection.inputs : connection.outputs;
            const ports = new Map<string, GraphPortEndpoint>();
            for (const matcher of matchers) {
                for (const resolved of resolveScopePorts(scope, matcher, connection.kind, direction)) {
                    const port: GraphPortEndpoint = { address: resolved.node.address, kind: connection.kind,
                        direction, index: resolved.port.index, name: resolved.port.name };
                    ports.set(portEndpointKey(port), port);
                }
            }
            const visible = new Map<string, { anchor: PortWireAnchor; selected: boolean; revealed: boolean }>();
            for (const port of ports.values()) {
                const anchor = anchorFor(port, entry.id);
                if (!anchor) continue;
                const isSelected = selected.has(addressKey(port.address));
                const isRevealed = revealed.get(entry.id)?.has(portEndpointKey(port)) ?? false;
                const existing = visible.get(anchor.id);
                if (existing) {
                    existing.selected ||= isSelected;
                    existing.revealed ||= isRevealed;
                } else visible.set(anchor.id, { anchor, selected: isSelected, revealed: isRevealed });
            }
            return [...visible.values()];
        };
        const outputs = resolve("output");
        const inputs = resolve("input");
        for (const output of outputs) {
            for (const input of inputs) {
                const source = output.anchor;
                const target = input.anchor;
                const emphasized = output.selected || input.selected;
                if (!emphasized && !output.revealed && !input.revealed && (!context || source.y < context.top || source.y > context.bottom
                    || target.y < context.top || target.y > context.bottom)) continue;
                const key = JSON.stringify([source.id, target.id]);
                const existing = wires.get(key);
                if (existing) {
                    if (!existing.connectionIds.includes(entry.id)) existing.connectionIds.push(entry.id);
                    existing.applied = existing.applied && entry.applied;
                    existing.emphasized ||= emphasized;
                } else {
                    wires.set(key, { key, source, target, connectionIds: [entry.id], applied: entry.applied, emphasized });
                }
            }
        }
    }
    return [...wires.values()];
}

export function selectedLaneWirePath(wire: SelectedLaneWire): string {
    return connectionWirePath(wire.source, wire.target);
}

export function connectionWirePath(first: Point, second: Point): string {
    const middleX = (first.x + second.x) / 2;
    return `M ${first.x} ${first.y} C ${middleX} ${first.y}, ${middleX} ${second.y}, ${second.x} ${second.y}`;
}
