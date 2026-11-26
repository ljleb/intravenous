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

export type SymbolicPortConnectivity =
    | "disconnected"
    | "connected"
    | "mixed";

export type SymbolicPortBinding =
    | "interface"
    | "none"
    | "symbolicInput"
    | "symbolicOutput"
    | "mixed";

export type FractionalGlobalIndexValue = {
    wholeBits: string;
    fractionBits: string;
};

export type ScalarPresentationNumber =
    | number
    | "nan"
    | "positiveInfinity"
    | "negativeInfinity";

export type ContinuousValueAlternative = {
    type: string;
    rank: number;
    channelCapability: "scalar" | "audio";
    defaultValue?: ScalarPresentationNumber | FractionalGlobalIndexValue;
    minValue?: ScalarPresentationNumber;
    maxValue?: ScalarPresentationNumber;
    scrubSpeed?: ScalarPresentationNumber;
};

export type SymbolicContinuousPort = {
    index: number;
    name: string;
    displayName?: string;
    shortDisplayName?: string;
    connectivity: SymbolicPortConnectivity;
    binding: SymbolicPortBinding;
    valueTypes: ContinuousValueAlternative[];
    channelType?: string;
};

export type SymbolicEventPort = {
    index: number;
    name: string;
    displayName?: string;
    shortDisplayName?: string;
    connectivity: SymbolicPortConnectivity;
    binding: SymbolicPortBinding;
    eventType: string;
};

export type SymbolicNodeMember = {
    index?: number;
    backingNodeId?: string;
    kind?: string;
    typeIdentity?: string;
    definitionId?: string;
    continuousInputs?: SymbolicContinuousPort[];
    continuousOutputs?: SymbolicContinuousPort[];
    eventInputs?: SymbolicEventPort[];
    eventOutputs?: SymbolicEventPort[];
};

export type SymbolicNode = {
    id?: string;
    instanceId?: string;
    kind?: string;
    sourceIdentity?: string;
    typeIdentity?: string;
    definitionId?: string;
    sourceSpans?: SourceSpan[];
    continuousInputs?: SymbolicContinuousPort[];
    continuousOutputs?: SymbolicContinuousPort[];
    eventInputs?: SymbolicEventPort[];
    eventOutputs?: SymbolicEventPort[];
    memberCount?: number;
    members?: SymbolicNodeMember[];
};
