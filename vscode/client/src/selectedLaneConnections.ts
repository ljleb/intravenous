import { addressKey, resolveScopePorts, type GraphScope, type NodeAddress } from "./graphScope";
import type { GraphPortEndpoint } from "./graphConnectionEditing";

export type PortWireAnchor = { id: string; x: number; y: number };
export type RevealedConnectionEnd = { connectionId: string; port: GraphPortEndpoint };
export type SelectedLaneWire = {
    key: string;
    source: PortWireAnchor;
    target: PortWireAnchor;
    connectionIds: string[];
    applied: boolean;
    // Defined only for connections whose visible ends belong to the same node.
    selfLoopY?: number;
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
export function selectedLaneConnections(scope: GraphScope, selected: string | null,
    anchorFor: (port: GraphPortEndpoint, connectionId: string) => PortWireAnchor | null,
    revealedEnds: readonly RevealedConnectionEnd[] = []): SelectedLaneWire[] {
    const node = scope.nodes.find((entry) => addressKey(entry.address) === selected);
    if (!node && !revealedEnds.length) return [];
    const revealed = new Map<string, Set<string>>();
    for (const end of revealedEnds) {
        const ports = revealed.get(end.connectionId) ?? new Set<string>();
        ports.add(portEndpointKey(end.port));
        revealed.set(end.connectionId, ports);
    }
    const wires = new Map<string, SelectedLaneWire>();
    for (const entry of scope.connections) {
        const { connection } = entry;
        if (!revealed.has(entry.id) && (!node || ![...connection.outputs, ...connection.inputs]
            .some((matcher) => matcher.instanceId === node.instance.instanceId))) continue;
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
            const visible = new Map<string, { anchor: PortWireAnchor; selected: boolean; revealed: boolean; nodeKey: string }>();
            for (const port of ports.values()) {
                const anchor = anchorFor(port, entry.id);
                if (!anchor) continue;
                const isSelected = addressKey(port.address) === selected;
                const isRevealed = revealed.get(entry.id)?.has(portEndpointKey(port)) ?? false;
                const existing = visible.get(anchor.id);
                if (existing) {
                    existing.selected ||= isSelected;
                    existing.revealed ||= isRevealed;
                } else visible.set(anchor.id, { anchor, selected: isSelected, revealed: isRevealed, nodeKey: addressKey(port.address) });
            }
            return [...visible.values()];
        };
        const outputs = resolve("output");
        const inputs = resolve("input");
        for (const output of outputs) {
            for (const input of inputs) {
                if (!output.selected && !input.selected && !output.revealed && !input.revealed) continue;
                const source = output.anchor;
                const target = input.anchor;
                const key = JSON.stringify([source.id, target.id]);
                const existing = wires.get(key);
                if (existing) {
                    if (!existing.connectionIds.includes(entry.id)) existing.connectionIds.push(entry.id);
                    existing.applied = existing.applied && entry.applied;
                } else {
                    wires.set(key, { key, source, target, connectionIds: [entry.id], applied: entry.applied,
                        ...(output.nodeKey === input.nodeKey ? { selfLoopY: Math.max(source.y, target.y) } : {}) });
                }
            }
        }
    }
    const result = [...wires.values()];
    const selfWires = result.filter((wire) => wire.selfLoopY !== undefined);
    const lowestPortY = selfWires.reduce((lowest, wire) => Math.max(lowest, wire.selfLoopY!), -Infinity);
    selfWires.forEach((wire, index) => { wire.selfLoopY = lowestPortY + 18 + index * 6; });
    return result;
}

export function selectedLaneWirePath(wire: SelectedLaneWire): string {
    const { source, target } = wire;
    if (wire.selfLoopY !== undefined || source.y === target.y) {
        // Keep a return arc for self-connections and equal-height endpoints;
        // a single collinear cubic would otherwise retrace the port row.
        const loopY = wire.selfLoopY ?? source.y + 24;
        const middleX = (source.x + target.x) / 2;
        const bend = Math.min(24, Math.abs(source.x - target.x) * 0.25 || 12);
        return `M ${source.x} ${source.y} C ${source.x - bend} ${source.y}, ${source.x - bend} ${loopY}, ${middleX} ${loopY}`
            + ` C ${target.x + bend} ${loopY}, ${target.x + bend} ${target.y}, ${target.x} ${target.y}`;
    }
    const bend = Math.min(100, Math.max(24, Math.abs(target.y - source.y) * 0.4));
    return `M ${source.x} ${source.y} C ${source.x - bend} ${source.y}, ${target.x + bend} ${target.y}, ${target.x} ${target.y}`;
}
