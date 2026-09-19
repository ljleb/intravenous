export type LiveGraphPortStateFamily =
    | "sampleInput"
    | "eventInput"
    | "sampleOutput"
    | "eventOutput";

export type LiveGraphPortStateAction = {
    state: string;
    label: string;
    reset?: boolean;
};

export type SerializedLiveGraphPort = {
    name: string;
    connectivity: string;
    ordinal: number;
    defaultValue: number;
    minValue: number | null;
    maxValue: number | null;
    currentValue: number;
    hasConcreteOverride: boolean;
    stateValue: string;
    tweakable: boolean;
    stateFamily: LiveGraphPortStateFamily | null;
    stateSummary: string;
    stateActions: LiveGraphPortStateAction[];
    resetState: string | null;
};

export type SerializedLiveGraphGroup = {
    label: string;
    count: number;
    direction: "input" | "output";
    portKind: "sample" | "event";
    ports: SerializedLiveGraphPort[];
};

export type SerializedLiveGraphMember = {
    ordinal: number;
    backingNodeId: string;
    kind: string;
    description: string;
    groups: SerializedLiveGraphGroup[];
};

export type SerializedLiveGraphNode = {
    id: string;
    instanceId: string;
    kind: string;
    description: string;
    tooltip: string;
    memberCount: number;
    icon: "merged" | "single";
    groups: SerializedLiveGraphGroup[];
    members: SerializedLiveGraphMember[];
};

export type SerializedLiveGraphInstance = {
    instanceId: string;
    definitionId: string;
    moduleId: string;
    packageRoot: string;
    realized: boolean;
    label: string;
};

export type LiveGraphSetInstancesMessage = {
    type: "setInstances";
    instances: SerializedLiveGraphInstance[];
};

export type LiveGraphSetSelectedInstanceMessage = {
    type: "setSelectedInstance";
    selectedInstanceId: string | null;
};

export type LiveGraphSetPackageRootMessage = {
    type: "setPackageRoot";
    packageRoot: string | null;
};

export type LiveGraphSetNodesMessage = {
    type: "setNodes";
    nodes: SerializedLiveGraphNode[];
};

export type LiveGraphUpsertNodesMessage = {
    type: "upsertNodes";
    replaceInstanceIds: string[];
    nodes: SerializedLiveGraphNode[];
};

export type LiveGraphSelectInstanceMessage = {
    type: "selectInstance";
    instanceId: string | null;
};

export type LiveGraphCreateInstanceMessage = {
    type: "createInstance";
};

export type LiveGraphControlMessage =
    | LiveGraphSelectInstanceMessage
    | LiveGraphCreateInstanceMessage;

export type LiveGraphControlHandler = (message: LiveGraphControlMessage) => Promise<void>;

export function isLiveGraphControlMessage(message: unknown): message is LiveGraphControlMessage {
    if (!message || typeof message !== "object") {
        return false;
    }

    const candidate = message as Record<string, unknown>;
    if (candidate.type === "selectInstance") {
        return candidate.instanceId == null || typeof candidate.instanceId === "string";
    }
    return candidate.type === "createInstance";
}
