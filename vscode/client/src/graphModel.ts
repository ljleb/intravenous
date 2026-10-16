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

export type VirtualPortBinding =
    | "interface"
    | "none"
    | "virtualInput"
    | "virtualOutput"
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
};

export type VirtualContinuousPort = {
    index: number;
    name: string;
    connectivity: VirtualPortConnectivity;
    binding: VirtualPortBinding;
    valueTypes: ContinuousValueAlternative[];
    channelType?: string;
};

export type VirtualEventPort = {
    index: number;
    name: string;
    connectivity: VirtualPortConnectivity;
    binding: VirtualPortBinding;
    eventType: string;
};

export type VirtualNodeMember = {
    index?: number;
    backingNodeId?: string;
    kind?: string;
    typeIdentity?: string;
    continuousInputs?: VirtualContinuousPort[];
    continuousOutputs?: VirtualContinuousPort[];
    eventInputs?: VirtualEventPort[];
    eventOutputs?: VirtualEventPort[];
};

export type VirtualNode = {
    id?: string;
    instanceId?: string;
    kind?: string;
    packageIdentity?: string;
    typeIdentity?: string;
    sourceSpans?: SourceSpan[];
    continuousInputs?: VirtualContinuousPort[];
    continuousOutputs?: VirtualContinuousPort[];
    eventInputs?: VirtualEventPort[];
    eventOutputs?: VirtualEventPort[];
    memberCount?: number;
    members?: VirtualNodeMember[];
};
