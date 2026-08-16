export type SourcePosition = {
    line: number;
    column: number;
};

export type SourceRange = {
    start: SourcePosition;
    end: SourcePosition;
};

export type SourceSpan = {
    filePath: string;
    start: SourcePosition;
    end: SourcePosition;
};

export type VirtualPortConnectivity =
    | "disconnected"
    | "connected"
    | "mixed";

export type VirtualPort = {
    ordinal?: number;
    name?: string;
    type?: string;
    connectivity?: VirtualPortConnectivity | string;
    defaultValue?: number;
    minValue?: number | null;
    maxValue?: number | null;
    currentValue?: number;
    hasConcreteOverride?: boolean;
    stateValue?: string;
};

export type VirtualNodeMember = {
    ordinal?: number;
    backingNodeId?: string;
    kind?: string;
    typeIdentity?: string;
    sampleInputs?: VirtualPort[];
    sampleOutputs?: VirtualPort[];
    eventInputs?: VirtualPort[];
    eventOutputs?: VirtualPort[];
};

export type VirtualNode = {
    id?: string;
    instanceId?: string;
    kind?: string;
    sourceIdentity?: string;
    typeIdentity?: string;
    sourceSpans?: SourceSpan[];
    sampleInputs?: VirtualPort[];
    sampleOutputs?: VirtualPort[];
    eventInputs?: VirtualPort[];
    eventOutputs?: VirtualPort[];
    memberCount?: number;
    members?: VirtualNodeMember[];
};
