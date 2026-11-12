import type {
    ContinuousValueAlternative,
    SourceSpan,
    VirtualContinuousPort,
    VirtualEventPort,
    VirtualNode,
    VirtualNodeMember,
    VirtualPortBinding,
    VirtualPortConnectivity,
} from "../../src/graphModel";

export type SemanticPortDirection = "input" | "output";

type SemanticPortFields = {
    direction: SemanticPortDirection;
    index: number;
    name: string;
    connectivity: VirtualPortConnectivity;
    binding: VirtualPortBinding;
};

export type SemanticContinuousPort = SemanticPortFields & {
    kind: "continuous";
    valueTypes: ContinuousValueAlternative[];
    channelType: string | null;
};

export type SemanticEventPort = SemanticPortFields & {
    kind: "event";
    eventType: string;
};

export type SemanticPort = SemanticContinuousPort | SemanticEventPort;

export type SemanticNodeMember = {
    index: number;
    kind: string;
    definitionId: string;
    ports: SemanticPort[];
};

// The node object a presentation reads through useNode(). It is built from
// the backend's source-introspection VirtualNode record. That record carries
// no NodeAddress, so this object has no `address` until the graph-editor
// projection supplies one (Vue client stage 2).
export type SemanticNode = {
    // Source-introspection virtual-node ID.
    id: string;
    instanceId: string;
    definitionId: string;
    kind: string;
    sourceIdentity: string;
    memberCount: number;
    // Inputs before outputs; continuous before event ports in each direction.
    ports: SemanticPort[];
    members: SemanticNodeMember[];
    sourceSpans: SourceSpan[];
};

type PortRecord = {
    continuousInputs?: VirtualContinuousPort[];
    continuousOutputs?: VirtualContinuousPort[];
    eventInputs?: VirtualEventPort[];
    eventOutputs?: VirtualEventPort[];
};

function continuousPorts(
    ports: VirtualContinuousPort[] | undefined,
    direction: SemanticPortDirection,
): SemanticContinuousPort[] {
    return (ports ?? []).map((port) => ({
        kind: "continuous",
        direction,
        index: port.index,
        name: port.name,
        connectivity: port.connectivity,
        binding: port.binding,
        valueTypes: port.valueTypes,
        channelType: port.channelType ?? null,
    }));
}

function eventPorts(
    ports: VirtualEventPort[] | undefined,
    direction: SemanticPortDirection,
): SemanticEventPort[] {
    return (ports ?? []).map((port) => ({
        kind: "event",
        direction,
        index: port.index,
        name: port.name,
        connectivity: port.connectivity,
        binding: port.binding,
        eventType: port.eventType,
    }));
}

function semanticPorts(record: PortRecord): SemanticPort[] {
    return [
        ...continuousPorts(record.continuousInputs, "input"),
        ...eventPorts(record.eventInputs, "input"),
        ...continuousPorts(record.continuousOutputs, "output"),
        ...eventPorts(record.eventOutputs, "output"),
    ];
}

function semanticMember(member: VirtualNodeMember, node: VirtualNode): SemanticNodeMember {
    return {
        index: member.index ?? 0,
        kind: member.kind || node.kind || "",
        definitionId: member.definitionId ?? "",
        ports: semanticPorts(member),
    };
}

export function semanticNodeFromVirtualNode(node: VirtualNode): SemanticNode {
    return {
        id: node.id ?? "",
        instanceId: node.instanceId ?? "",
        definitionId: node.definitionId ?? "",
        kind: node.kind || node.id || "",
        sourceIdentity: node.sourceIdentity ?? "",
        memberCount: node.memberCount ?? 0,
        ports: semanticPorts(node),
        members: (node.members ?? []).map((member) => semanticMember(member, node)),
        sourceSpans: node.sourceSpans ?? [],
    };
}
