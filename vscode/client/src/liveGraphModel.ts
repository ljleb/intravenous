import { VirtualNode, VirtualNodeMember, VirtualPort } from "./graphModel";
import {
    SerializedLiveGraphInstance,
    LiveGraphPortStateAction,
    LiveGraphPortStateFamily,
    SerializedLiveGraphGroup,
    SerializedLiveGraphNode,
    SerializedLiveGraphPort,
} from "./liveGraphProtocol";

function virtualIdentitySummary(node: VirtualNode): string {
    const identity = typeof node?.sourceIdentity === "string" && node.sourceIdentity.length > 0
        ? node.sourceIdentity
        : (typeof node?.id === "string" ? node.id : "");
    if (!identity) {
        return "";
    }

    const matches = [...identity.matchAll(/@([^@#:$]+)$/g)];
    if (matches.length > 0 && matches[0][1]) {
        return matches[0][1];
    }

    const atIndex = identity.lastIndexOf("@");
    if (atIndex >= 0 && atIndex + 1 < identity.length) {
        return identity.slice(atIndex + 1);
    }

    return identity;
}

function sampleInputsForMemberView(node: VirtualNode, member: VirtualNodeMember): VirtualPort[] {
    const virtualInputs = new Map<number, VirtualPort>();
    for (const input of node.sampleInputs || []) {
        virtualInputs.set(Number(input.ordinal), input);
    }
    return (member.sampleInputs || []).map((input) => {
        if (input.hasConcreteOverride) {
            return input;
        }
        const virtualInput = virtualInputs.get(Number(input.ordinal));
        if (!virtualInput || typeof virtualInput.currentValue !== "number") {
            return input;
        }
        return {
            ...input,
            currentValue: virtualInput.currentValue,
        };
    });
}

function displayStateSummary(stateValue: string, family: LiveGraphPortStateFamily | null): string {
    switch (stateValue) {
    case "default":
        return "default";
    case "overridden":
        return family === "sampleInput" ? "override" : "default";
    case "virtualFollow":
        return "follow virtual";
    case "virtual":
        return "follow virtual output";
    case "timelineLane":
        return "lane-connected";
    case "disconnected":
        return "disconnected";
    default:
        return stateValue;
    }
}

type PortUiContext = {
    builtInConnected: boolean;
    virtualOutputAvailable: boolean;
    publicInput: boolean;
};

function laneAction(): LiveGraphPortStateAction {
    return { state: "timelineLane", label: "Connect lane" };
}

function virtualSampleInputUi(stateValue: string): { summary: string; actions: LiveGraphPortStateAction[] } {
    return {
        summary: stateValue === "timelineLane" ? "lane-connected" : "knob value",
        actions: stateValue === "timelineLane"
            ? [{ state: "overridden", label: "Use knob value" }]
            : [laneAction()],
    };
}

function virtualEventInputUi(stateValue: string, publicInput: boolean): { summary: string; actions: LiveGraphPortStateAction[] } {
    const summary =
        stateValue === "timelineLane" ? "lane-connected"
            : stateValue === "disconnected" ? "disconnected"
                : stateValue === "virtualFollow" ? "follow virtual input"
                    : "default";
    if (publicInput) {
        return {
            summary,
            actions: stateValue === "disconnected"
                ? [{ state: "default", label: "Connect lane" }]
                : [{ state: "disconnected", label: "Disconnect" }],
        };
    }

    const actions: LiveGraphPortStateAction[] = [];
    if (stateValue !== "default") {
        actions.push({ state: "default", label: "Use default behavior", reset: true });
    }
    if (stateValue !== "timelineLane") {
        actions.push(laneAction());
    }
    return { summary, actions };
}

function concreteSampleInputUi(
    stateValue: string,
    context: PortUiContext,
): { summary: string; actions: LiveGraphPortStateAction[] } {
    if (!context.builtInConnected) {
        const followVirtual: LiveGraphPortStateAction = {
            state: "default",
            label: "Follow virtual value",
            reset: true,
        };
        const override: LiveGraphPortStateAction = {
            state: "overridden",
            label: "Use concrete override",
        };
        if (stateValue === "timelineLane") {
            return {
                summary: "lane-connected",
                actions: [followVirtual, override, { state: "disconnected", label: "Disconnect" }],
            };
        }
        if (stateValue === "disconnected") {
            return {
                summary: "disconnected",
                actions: [followVirtual, override, laneAction()],
            };
        }
        if (stateValue === "overridden") {
            return {
                summary: "concrete override",
                actions: [followVirtual, laneAction(), { state: "disconnected", label: "Disconnect" }],
            };
        }
        return {
            summary: "follow virtual value",
            actions: [{ state: "disconnected", label: "Disconnect" }, override, laneAction()],
        };
    }

    const actions: LiveGraphPortStateAction[] = [];
    if (stateValue !== "disconnected") {
        actions.push({ state: "default", label: "Use built-in connection", reset: true });
    }
    if (stateValue !== "overridden") {
        actions.push({ state: "overridden", label: "Use concrete override" });
    }
    if (stateValue !== "virtualFollow") {
        actions.push({
            state: context.builtInConnected ? "virtualFollow" : "default",
            label: "Follow virtual value",
            reset: !context.builtInConnected,
        });
    }
    if (stateValue !== "timelineLane") {
        actions.push(laneAction());
    }
    if (context.builtInConnected) {
        return {
            summary:
                stateValue === "disconnected" ? "built-in connection"
                    : stateValue === "overridden" ? "concrete override"
                        : stateValue === "virtualFollow" ? "follow virtual value"
                            : "lane-connected",
            actions,
        };
    }
    if (stateValue !== "disconnected") {
        actions.push({ state: "disconnected", label: "Disconnect" });
    }
    return {
        summary:
            stateValue === "overridden" ? "concrete override"
                : stateValue === "virtualFollow" ? "follow virtual value"
                    : stateValue === "timelineLane" ? "lane-connected"
                        : "disconnected",
        actions,
    };
}

function concreteEventInputUi(
    stateValue: string,
    context: PortUiContext,
): { summary: string; actions: LiveGraphPortStateAction[] } {
    if (!context.builtInConnected) {
        const followVirtual: LiveGraphPortStateAction = {
            state: "default",
            label: "Follow virtual",
            reset: true,
        };
        if (stateValue === "timelineLane") {
            return {
                summary: "lane-connected",
                actions: [
                    followVirtual,
                    { state: "disconnected", label: "Disconnect" },
                ],
            };
        }
        if (stateValue === "disconnected") {
            return {
                summary: "disconnected",
                actions: [laneAction(), followVirtual],
            };
        }
        return {
            summary: "follow virtual input",
            actions: [
                { state: "disconnected", label: "Disconnect" },
                laneAction(),
            ],
        };
    }

    const actions: LiveGraphPortStateAction[] = [];
    if (context.builtInConnected && stateValue !== "disconnected") {
        actions.push({ state: "default", label: "Use built-in connection", reset: true });
    }
    if (stateValue !== "virtualFollow") {
        actions.push({
            state: context.builtInConnected ? "virtualFollow" : "default",
            label: "Follow virtual input",
            reset: !context.builtInConnected,
        });
    }
    if (stateValue !== "timelineLane") {
        actions.push(laneAction());
    }
    if (context.builtInConnected) {
        return {
            summary:
                stateValue === "disconnected" ? "built-in connection"
                    : stateValue === "virtualFollow" ? "follow virtual input"
                        : "lane-connected",
            actions,
        };
    }
    if (stateValue !== "disconnected") {
        actions.push({ state: "disconnected", label: "Disconnect" });
    }
    return {
        summary:
            stateValue === "virtualFollow" ? "follow virtual input"
                : stateValue === "timelineLane" ? "lane-connected"
                    : "disconnected",
        actions,
    };
}

function virtualOutputUi(stateValue: string): { summary: string; actions: LiveGraphPortStateAction[] } {
    return {
        summary: stateValue === "timelineLane" ? "lane-connected" : "disconnected",
        actions: stateValue === "timelineLane"
            ? [{ state: "disconnected", label: "Disconnect lane" }]
            : [laneAction()],
    };
}

function concreteOutputUi(
    stateValue: string,
    context: PortUiContext,
): { summary: string; actions: LiveGraphPortStateAction[] } {
    const virtualAction: LiveGraphPortStateAction = {
        state: "virtual",
        label: "Use virtual output",
    };
    if (stateValue === "timelineLane") {
        return {
            summary: "lane-connected",
            actions: [
                ...(context.virtualOutputAvailable ? [virtualAction] : []),
                { state: "disconnected", label: "Disconnect" },
            ],
        };
    }
    if (stateValue === "virtual") {
        return {
            summary: "virtual output",
            actions: [{ state: "disconnected", label: "Disconnect" }, laneAction()],
        };
    }
    return {
        summary: "disconnected",
        actions: [
            ...(context.virtualOutputAvailable ? [virtualAction] : []),
            laneAction(),
        ],
    };

}

function describePortState(
    port: VirtualPort,
    family: LiveGraphPortStateFamily | null,
    memberOrdinal: number | null,
    context: PortUiContext,
): { summary: string; resetState: string | null; actions: LiveGraphPortStateAction[] } {
    const connectivity = typeof port.connectivity === "string" ? port.connectivity : "disconnected";
    const stateValue = typeof port.stateValue === "string" && port.stateValue.length > 0
        ? port.stateValue
        : connectivity;

    if (family === "sampleInput") {
        if (memberOrdinal == null) {
            const state = virtualSampleInputUi(stateValue);
            return { summary: state.summary, resetState: null, actions: state.actions };
        }
        const state = concreteSampleInputUi(stateValue, context);
        return { summary: state.summary, resetState: null, actions: state.actions };
    }

    if (family === "eventInput") {
        if (memberOrdinal == null) {
            const state = virtualEventInputUi(stateValue, context.publicInput);
            return { summary: state.summary, resetState: null, actions: state.actions };
        }
        const state = concreteEventInputUi(stateValue, context);
        return { summary: state.summary, resetState: null, actions: state.actions };
    }

    if (family === "sampleOutput" || family === "eventOutput") {
        if (memberOrdinal == null) {
            const state = virtualOutputUi(stateValue);
            return { summary: state.summary, resetState: null, actions: state.actions };
        }
        const state = concreteOutputUi(stateValue, context);
        return { summary: state.summary, resetState: null, actions: state.actions };
    }

    return {
        summary: displayStateSummary(stateValue, family),
        resetState: null,
        actions: [],
    };
}

function serializePort(
    port: VirtualPort,
    index: number,
    direction: "input" | "output",
    portKind: "sample" | "event",
    memberOrdinal: number | null,
    context: PortUiContext,
    forceTweakable = false,
): SerializedLiveGraphPort {
    const stateFamily: LiveGraphPortStateFamily | null =
        direction === "input" && portKind === "sample" ? "sampleInput"
            : direction === "input" && portKind === "event" ? "eventInput"
                : direction === "output" && portKind === "sample" ? "sampleOutput"
                    : direction === "output" && portKind === "event" ? "eventOutput"
                        : null;
    const connectivity = typeof port.connectivity === "string" ? port.connectivity : "disconnected";
    const stateValue = typeof port.stateValue === "string" && port.stateValue.length > 0
        ? port.stateValue
        : connectivity;
    const tweakable = (forceTweakable || direction === "input")
        && direction === "input"
        && portKind === "sample"
        && stateValue !== "timelineLane"
        && stateValue !== "disconnected";
    const state = describePortState(port, stateFamily, memberOrdinal, context);

    return {
        name: port.name || `[${index}]`,
        connectivity,
        ordinal: Number.isInteger(port.ordinal) ? Number(port.ordinal) : index,
        defaultValue: typeof port.defaultValue === "number" ? port.defaultValue : 0,
        minValue: typeof port.minValue === "number" ? port.minValue : null,
        maxValue: typeof port.maxValue === "number" ? port.maxValue : null,
        currentValue: typeof port.currentValue === "number" ? port.currentValue : 0,
        hasConcreteOverride: Boolean(port.hasConcreteOverride),
        stateValue,
        tweakable,
        stateFamily,
        stateSummary: state.summary,
        stateActions: state.actions,
        resetState: state.resetState,
    };
}

function makePortGroup(
    label: string,
    ports: VirtualPort[] | undefined,
    direction: "input" | "output",
    portKind: "sample" | "event",
    memberOrdinal: number | null,
    contextForPort: ((port: VirtualPort) => PortUiContext) | null,
    forceTweakable = false,
): SerializedLiveGraphGroup | null {
    if (!Array.isArray(ports) || ports.length === 0) {
        return null;
    }

    return {
        label,
        count: ports.length,
        direction,
        portKind,
        ports: ports.map((port, index) => serializePort(
            port,
            index,
            direction,
            portKind,
            memberOrdinal,
            contextForPort ? contextForPort(port) : { builtInConnected: false, virtualOutputAvailable: false, publicInput: false },
            forceTweakable,
        )),
    };
}

function findVirtualPort(
    ports: VirtualPort[] | undefined,
    ordinal: number,
): VirtualPort | null {
    if (!Array.isArray(ports)) {
        return null;
    }
    return ports.find((port) => Number(port.ordinal) === Number(ordinal)) || null;
}

export function serializeLiveGraphNodes(nodes: VirtualNode[]): SerializedLiveGraphNode[] {
    return nodes.map((node) => ({
        id: node.id || "",
        instanceId: node.instanceId || "",
        kind: node.kind || node.id || "",
        description: Number(node.memberCount || 0) > 1
            ? (() => {
                const identity = virtualIdentitySummary(node);
                return identity ? `${identity} • ${node.memberCount} nodes` : `${node.memberCount} nodes`;
            })()
            : (virtualIdentitySummary(node) || node.id || ""),
        tooltip: `${node.kind || "node"}${virtualIdentitySummary(node) ? ` • ${virtualIdentitySummary(node)}` : ""}${Number(node.memberCount || 0) > 1 ? ` • ${node.memberCount} members` : ""}`,
        memberCount: Number(node.memberCount || 0),
        icon: Number(node.memberCount || 0) > 1 ? "merged" : "single",
        groups: [
            makePortGroup("sample inputs", node.sampleInputs, "input", "sample", null, null),
            makePortGroup("sample outputs", node.sampleOutputs, "output", "sample", null, null),
            makePortGroup("event inputs", node.eventInputs, "input", "event", null, () => ({
                builtInConnected: false,
                virtualOutputAvailable: false,
                publicInput: String(node.id || "").startsWith("iv.public-input:"),
            })),
            makePortGroup("event outputs", node.eventOutputs, "output", "event", null, null),
        ].filter((group): group is SerializedLiveGraphGroup => group !== null),
        members: Array.isArray(node.members)
            ? node.members.map((member) => ({
                ordinal: Number(member.ordinal || 0),
                backingNodeId: member.backingNodeId || "",
                kind: member.kind || node.kind || "",
                description: `member ${Number(member.ordinal || 0)}`,
                groups: [
                    makePortGroup(
                        "sample inputs",
                        sampleInputsForMemberView(node, member),
                        "input",
                        "sample",
                        Number(member.ordinal || 0),
                        (port) => ({
                            builtInConnected: port.connectivity === "connected",
                            virtualOutputAvailable: false,
                            publicInput: false,
                        }),
                        true),
                    makePortGroup(
                        "sample outputs",
                        member.sampleOutputs,
                        "output",
                        "sample",
                        Number(member.ordinal || 0),
                        (port) => ({
                            builtInConnected: false,
                            virtualOutputAvailable:
                                findVirtualPort(node.sampleOutputs, Number(port.ordinal))?.stateValue === "timelineLane",
                            publicInput: false,
                        })),
                    makePortGroup(
                        "event inputs",
                        member.eventInputs,
                        "input",
                        "event",
                        Number(member.ordinal || 0),
                        (port) => ({
                            builtInConnected: port.connectivity === "connected",
                            virtualOutputAvailable: false,
                            publicInput: false,
                        })),
                    makePortGroup(
                        "event outputs",
                        member.eventOutputs,
                        "output",
                        "event",
                        Number(member.ordinal || 0),
                        (port) => ({
                            builtInConnected: false,
                            virtualOutputAvailable:
                                findVirtualPort(node.eventOutputs, Number(port.ordinal))?.stateValue === "timelineLane",
                            publicInput: false,
                        })),
                ].filter((group): group is SerializedLiveGraphGroup => group !== null),
            }))
            : [],
    }));
}

export type LiveGraphInstance = {
    instanceId: string;
    definitionId?: string;
    displayName?: string;
    moduleId?: string;
    moduleRoot?: string;
    realized?: boolean;
};

export function serializeLiveGraphInstances(instances: LiveGraphInstance[]): SerializedLiveGraphInstance[] {
    return instances.map((instance) => {
        const moduleId = instance.moduleId || "";
        const instanceId = instance.instanceId || "";
        return {
            instanceId,
            definitionId: instance.definitionId || "",
            moduleId,
            moduleRoot: instance.moduleRoot || "",
            realized: Boolean(instance.realized),
            label: instance.displayName || (moduleId ? `${moduleId} • ${instanceId}` : instanceId),
        };
    });
}
