import type { VirtualPortBinding, VirtualPortConnectivity } from "../../src/graphModel";
import type { SemanticNode, SemanticPort } from "./semanticNode";

// Display text derived from a SemanticNode, shared by presentations.

export function portKey(port: SemanticPort): string {
    return `${port.direction}:${port.kind}:${port.index}`;
}

export type PortGroup = {
    label: string;
    ports: SemanticPort[];
};

export function portGroups(ports: readonly SemanticPort[]): PortGroup[] {
    const groups: PortGroup[] = [
        { label: "continuous inputs", ports: [] },
        { label: "event inputs", ports: [] },
        { label: "continuous outputs", ports: [] },
        { label: "event outputs", ports: [] },
    ];
    for (const port of ports) {
        const group = port.direction === "input"
            ? (port.kind === "continuous" ? groups[0] : groups[1])
            : (port.kind === "continuous" ? groups[2] : groups[3]);
        group.ports.push(port);
    }
    return groups.filter((group) => group.ports.length > 0);
}

export function bindingSummary(
    binding: VirtualPortBinding,
    connectivity: VirtualPortConnectivity,
): string {
    switch (binding) {
    case "interface":
        return "graph interface";
    case "virtualInput":
        return "follow virtual input";
    case "virtualOutput":
        return "virtual output";
    case "mixed":
        return "mixed built-in and virtual input";
    case "none":
        return connectivity === "connected" ? "built-in connection"
            : connectivity === "mixed" ? "mixed connections"
                : "disconnected";
    }
}

export function portTypeSummary(port: SemanticPort): string {
    if (port.kind === "event") {
        return port.eventType;
    }
    const types = port.valueTypes.map((value) => value.type).join(" | ");
    if (!port.channelType) {
        return types;
    }
    return types.length > 0 ? `${types} • ${port.channelType}` : port.channelType;
}

// Type, binding, and (when the binding summary does not already state it)
// connectivity.
export function portDescription(port: SemanticPort): string {
    const parts = [
        portTypeSummary(port),
        bindingSummary(port.binding, port.connectivity),
    ];
    if (port.binding !== "none") {
        parts.push(port.connectivity);
    }
    return parts.filter((part) => part.length > 0).join(" • ");
}

// The short source identity after the last '@' of the node's source
// identity, or of its ID when the source identity is empty.
export function nodeIdentitySummary(node: SemanticNode): string {
    if (node.address) return "";
    const identity = node.sourceIdentity || node.id;
    if (!identity) {
        return "";
    }
    const match = /@([^@#:$]+)$/.exec(identity);
    if (match?.[1]) {
        return match[1];
    }
    const at = identity.lastIndexOf("@");
    return at >= 0 && at + 1 < identity.length ? identity.slice(at + 1) : identity;
}

export function nodeDescription(node: SemanticNode): string {
    if (node.address) return node.memberCount > 1 ? `${node.memberCount} concrete nodes` : "";
    const identity = nodeIdentitySummary(node);
    const parts = [node.address ? "instance" : "virtual"];
    if (identity) {
        parts.push(identity);
    }
    if (node.memberCount > 1) {
        parts.push(`${node.memberCount} concrete nodes`);
    }
    return parts.join(" • ");
}

export function nodeDisplayName(node: SemanticNode): string {
    if (node.displayName?.trim()) return node.displayName;
    if (node.address && node.definitionId) return node.definitionId.split(".").filter(Boolean).at(-1) ?? node.definitionId;
    return node.kind;
}
