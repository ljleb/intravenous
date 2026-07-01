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
    currentValue: number;
    hasConcreteOverride: boolean;
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
    kind: string;
    description: string;
    tooltip: string;
    memberCount: number;
    icon: "merged" | "single";
    groups: SerializedLiveGraphGroup[];
    members: SerializedLiveGraphMember[];
};

export type LiveGraphSetStateMessage = {
    type: "setState";
    nodes: SerializedLiveGraphNode[];
};

export type LiveGraphSetSampleInputValueMessage = {
    type: "setSampleInputValue";
    nodeId: string;
    inputOrdinal: number;
    value: unknown;
    memberOrdinal?: number | null;
};

export type LiveGraphSetSampleInputStateMessage = {
    type: "setSampleInputState";
    nodeId: string;
    inputOrdinal: number;
    state: string;
    memberOrdinal?: number | null;
};

export type LiveGraphSetEventInputStateMessage = {
    type: "setEventInputState";
    nodeId: string;
    inputOrdinal: number;
    state: string;
    memberOrdinal?: number | null;
};

export type LiveGraphSetSampleOutputStateMessage = {
    type: "setSampleOutputState";
    nodeId: string;
    outputOrdinal: number;
    state: string;
    memberOrdinal?: number | null;
};

export type LiveGraphSetEventOutputStateMessage = {
    type: "setEventOutputState";
    nodeId: string;
    outputOrdinal: number;
    state: string;
    memberOrdinal?: number | null;
};

export type LiveGraphControlMessage =
    | LiveGraphSetSampleInputValueMessage
    | LiveGraphSetSampleInputStateMessage
    | LiveGraphSetEventInputStateMessage
    | LiveGraphSetSampleOutputStateMessage
    | LiveGraphSetEventOutputStateMessage;

export type LiveGraphControlHandler = (message: LiveGraphControlMessage) => Promise<void>;

export function isLiveGraphControlMessage(message: unknown): message is LiveGraphControlMessage {
    if (!message || typeof message !== "object") {
        return false;
    }

    const candidate = message as Record<string, unknown>;
    if (typeof candidate.type !== "string" || typeof candidate.nodeId !== "string") {
        return false;
    }

    switch (candidate.type) {
    case "setSampleInputValue":
        return typeof candidate.inputOrdinal === "number";
    case "setSampleInputState":
    case "setEventInputState":
        return typeof candidate.inputOrdinal === "number" && typeof candidate.state === "string";
    case "setSampleOutputState":
    case "setEventOutputState":
        return typeof candidate.outputOrdinal === "number" && typeof candidate.state === "string";
    default:
        return false;
    }
}
