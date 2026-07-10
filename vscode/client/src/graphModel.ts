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

export type LogicalPortConnectivity =
    | "disconnected"
    | "connected"
    | "mixed";

export type LogicalPort = {
    ordinal?: number;
    name?: string;
    type?: string;
    connectivity?: LogicalPortConnectivity | string;
    defaultValue?: number;
    currentValue?: number;
    hasConcreteOverride?: boolean;
    stateValue?: string;
};

export type LogicalNodeMember = {
    ordinal?: number;
    backingNodeId?: string;
    kind?: string;
    typeIdentity?: string;
    sampleInputs?: LogicalPort[];
    sampleOutputs?: LogicalPort[];
    eventInputs?: LogicalPort[];
    eventOutputs?: LogicalPort[];
};

export type LogicalNode = {
    id?: string;
    instanceId?: string;
    kind?: string;
    sourceIdentity?: string;
    typeIdentity?: string;
    sourceSpans?: SourceSpan[];
    sampleInputs?: LogicalPort[];
    sampleOutputs?: LogicalPort[];
    eventInputs?: LogicalPort[];
    eventOutputs?: LogicalPort[];
    memberCount?: number;
    members?: LogicalNodeMember[];
};
