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
import type { GraphScope, GraphScopeNode, GraphScopePort, NodeAddress } from "../../src/graphScope";
import { resolveScopePorts } from "../../src/graphScope";

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

// The node object a presentation reads through useNode(). Source inspection
// supplies VirtualNode metadata; the graph editor supplies a stable address
// and the desired instance's ports from GraphScope.
export type SemanticNode = {
    loadState?: GraphScopeNode["loadState"];
    displayName?: string;
    realized?: boolean;
    errorMessage?: string;
    address?: NodeAddress;
    // Present only when this presentation hosts an opened graph scope.
    scope?: GraphScope;
    // Source-introspection virtual-node ID or project instance ID.
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

export function semanticNodeFromScopeChild(child: GraphScopeNode, scope: GraphScope): SemanticNode {
    const port = (entry: GraphScopePort, kind: "continuous" | "event", direction: SemanticPortDirection): SemanticPort => {
        const connected = scope.connections.some(({ connection }) => {
            if (connection.kind !== (kind === "continuous" ? "sample" : "event")) return false;
            const matchers = direction === "input" ? connection.inputs : connection.outputs;
            return matchers.some((matcher) => {
                return resolveScopePorts(scope, matcher, connection.kind, direction)
                    .some((resolved) => resolved.node === child && resolved.port.index === entry.index);
            });
        });
        const base: SemanticPortFields = {
            index: entry.index, name: entry.name, direction,
            connectivity: connected ? "connected" : "disconnected", binding: "interface",
        };
        return kind === "continuous"
            ? { ...base, kind, valueTypes: entry.valueTypes ?? [], channelType: entry.channelType ?? null }
            : { ...base, kind, eventType: entry.eventType ?? "" };
    };
    return {
        address: child.address, id: child.instance.instanceId,
        displayName: child.instance.displayName,
        realized: child.instance.realized,
        loadState: child.loadState,
        errorMessage: child.errorMessage,
        instanceId: child.instance.instanceId, definitionId: child.instance.definitionId,
        kind: child.instance.definitionId, sourceIdentity: "", memberCount: 1,
        members: [], sourceSpans: [],
        ports: [
            ...child.sampleInputs.map((entry) => port(entry, "continuous", "input")),
            ...child.eventInputs.map((entry) => port(entry, "event", "input")),
            ...child.sampleOutputs.map((entry) => port(entry, "continuous", "output")),
            ...child.eventOutputs.map((entry) => port(entry, "event", "output")),
        ],
    };
}

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
