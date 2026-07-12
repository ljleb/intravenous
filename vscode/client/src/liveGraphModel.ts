import { LogicalNode, LogicalNodeMember, LogicalPort } from "./graphModel";
import {
    SerializedLiveGraphInstance,
    LiveGraphPortStateAction,
    LiveGraphPortStateFamily,
    SerializedLiveGraphGroup,
    SerializedLiveGraphNode,
    SerializedLiveGraphPort,
} from "./liveGraphProtocol";

function logicalIdentitySummary(node: LogicalNode): string {
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

function sampleInputsForMemberView(node: LogicalNode, member: LogicalNodeMember): LogicalPort[] {
    const logicalInputs = new Map<number, LogicalPort>();
    for (const input of node.sampleInputs || []) {
        logicalInputs.set(Number(input.ordinal), input);
    }
    return (member.sampleInputs || []).map((input) => {
        if (input.hasConcreteOverride) {
            return input;
        }
        const logicalInput = logicalInputs.get(Number(input.ordinal));
        if (!logicalInput || typeof logicalInput.currentValue !== "number") {
            return input;
        }
        return {
            ...input,
            currentValue: logicalInput.currentValue,
        };
    });
}

function displayStateSummary(stateValue: string, family: LiveGraphPortStateFamily | null): string {
    switch (stateValue) {
    case "default":
        return "default";
    case "overridden":
        return family === "sampleInput" ? "override" : "default";
    case "logicalFollow":
        return "follow logical";
    case "logical":
        return "follow logical output";
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
    logicalOutputAvailable: boolean;
};

function laneAction(): LiveGraphPortStateAction {
    return { state: "timelineLane", label: "Connect lane" };
}

function logicalSampleInputUi(stateValue: string): { summary: string; actions: LiveGraphPortStateAction[] } {
    return {
        summary: stateValue === "timelineLane" ? "lane-connected" : "knob value",
        actions: stateValue === "timelineLane"
            ? [{ state: "overridden", label: "Use knob value" }]
            : [laneAction()],
    };
}

function logicalEventInputUi(stateValue: string): { summary: string; actions: LiveGraphPortStateAction[] } {
    const summary =
        stateValue === "timelineLane" ? "lane-connected"
            : stateValue === "disconnected" ? "disconnected"
                : stateValue === "logicalFollow" ? "follow logical input"
                    : "default";
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
    const actions: LiveGraphPortStateAction[] = [];
    if (stateValue !== "overridden") {
        actions.push({ state: "overridden", label: "Use concrete override" });
    }
    if (stateValue !== "logicalFollow") {
        actions.push({
            state: context.builtInConnected ? "logicalFollow" : "default",
            label: "Follow logical value",
            reset: !context.builtInConnected,
        });
    }
    if (stateValue !== "timelineLane") {
        actions.push(laneAction());
    }
    if (context.builtInConnected) {
        if (stateValue !== "disconnected") {
            actions.push({ state: "default", label: "Use built-in connection", reset: true });
        }
        return {
            summary:
                stateValue === "disconnected" ? "built-in connection"
                    : stateValue === "overridden" ? "concrete override"
                        : stateValue === "logicalFollow" ? "follow logical value"
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
                : stateValue === "logicalFollow" ? "follow logical value"
                    : stateValue === "timelineLane" ? "lane-connected"
                        : "disconnected",
        actions,
    };
}

function concreteEventInputUi(
    stateValue: string,
    context: PortUiContext,
): { summary: string; actions: LiveGraphPortStateAction[] } {
    const actions: LiveGraphPortStateAction[] = [];
    if (stateValue !== "logicalFollow") {
        actions.push({
            state: context.builtInConnected ? "logicalFollow" : "default",
            label: "Follow logical input",
            reset: !context.builtInConnected,
        });
    }
    if (stateValue !== "timelineLane") {
        actions.push(laneAction());
    }
    if (context.builtInConnected) {
        if (stateValue !== "disconnected") {
            actions.push({ state: "default", label: "Use built-in connection", reset: true });
        }
        return {
            summary:
                stateValue === "disconnected" ? "built-in connection"
                    : stateValue === "logicalFollow" ? "follow logical input"
                        : "lane-connected",
            actions,
        };
    }
    if (stateValue !== "disconnected") {
        actions.push({ state: "disconnected", label: "Disconnect" });
    }
    return {
        summary:
            stateValue === "logicalFollow" ? "follow logical input"
                : stateValue === "timelineLane" ? "lane-connected"
                    : "disconnected",
        actions,
    };
}

function logicalOutputUi(stateValue: string): { summary: string; actions: LiveGraphPortStateAction[] } {
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
    const actions: LiveGraphPortStateAction[] = [];
    if ((context.logicalOutputAvailable || stateValue === "logical") && stateValue !== "logical") {
        actions.push({ state: "logical", label: "Use logical output" });
    }
    if (stateValue !== "timelineLane") {
        actions.push(laneAction());
    }
    if (stateValue !== "disconnected") {
        actions.push({ state: "disconnected", label: "Disconnect" });
    }
    return {
        summary:
            stateValue === "logical" ? "logical output"
                : stateValue === "timelineLane" ? "lane-connected"
                    : "disconnected",
        actions,
    };
}

function describePortState(
    port: LogicalPort,
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
            const state = logicalSampleInputUi(stateValue);
            return { summary: state.summary, resetState: null, actions: state.actions };
        }
        const state = concreteSampleInputUi(stateValue, context);
        return { summary: state.summary, resetState: null, actions: state.actions };
    }

    if (family === "eventInput") {
        if (memberOrdinal == null) {
            const state = logicalEventInputUi(stateValue);
            return { summary: state.summary, resetState: null, actions: state.actions };
        }
        const state = concreteEventInputUi(stateValue, context);
        return { summary: state.summary, resetState: null, actions: state.actions };
    }

    if (family === "sampleOutput" || family === "eventOutput") {
        if (memberOrdinal == null) {
            const state = logicalOutputUi(stateValue);
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
    port: LogicalPort,
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
    ports: LogicalPort[] | undefined,
    direction: "input" | "output",
    portKind: "sample" | "event",
    memberOrdinal: number | null,
    contextForPort: ((port: LogicalPort) => PortUiContext) | null,
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
            contextForPort ? contextForPort(port) : { builtInConnected: false, logicalOutputAvailable: false },
            forceTweakable,
        )),
    };
}

function findLogicalPort(
    ports: LogicalPort[] | undefined,
    ordinal: number,
): LogicalPort | null {
    if (!Array.isArray(ports)) {
        return null;
    }
    return ports.find((port) => Number(port.ordinal) === Number(ordinal)) || null;
}

export function serializeLiveGraphNodes(nodes: LogicalNode[]): SerializedLiveGraphNode[] {
    return nodes.map((node) => ({
        id: node.id || "",
        instanceId: node.instanceId || "",
        kind: node.kind || node.id || "",
        description: Number(node.memberCount || 0) > 1
            ? (() => {
                const identity = logicalIdentitySummary(node);
                return identity ? `${identity} • ${node.memberCount} nodes` : `${node.memberCount} nodes`;
            })()
            : (logicalIdentitySummary(node) || node.id || ""),
        tooltip: `${node.kind || "node"}${logicalIdentitySummary(node) ? ` • ${logicalIdentitySummary(node)}` : ""}${Number(node.memberCount || 0) > 1 ? ` • ${node.memberCount} members` : ""}`,
        memberCount: Number(node.memberCount || 0),
        icon: Number(node.memberCount || 0) > 1 ? "merged" : "single",
        groups: [
            makePortGroup("sample inputs", node.sampleInputs, "input", "sample", null, null),
            makePortGroup("sample outputs", node.sampleOutputs, "output", "sample", null, null),
            makePortGroup("event inputs", node.eventInputs, "input", "event", null, null),
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
                            logicalOutputAvailable: false,
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
                            logicalOutputAvailable:
                                findLogicalPort(node.sampleOutputs, Number(port.ordinal))?.stateValue === "timelineLane",
                        })),
                    makePortGroup(
                        "event inputs",
                        member.eventInputs,
                        "input",
                        "event",
                        Number(member.ordinal || 0),
                        (port) => ({
                            builtInConnected: port.connectivity === "connected",
                            logicalOutputAvailable: false,
                        })),
                    makePortGroup(
                        "event outputs",
                        member.eventOutputs,
                        "output",
                        "event",
                        Number(member.ordinal || 0),
                        (port) => ({
                            builtInConnected: false,
                            logicalOutputAvailable:
                                findLogicalPort(node.eventOutputs, Number(port.ordinal))?.stateValue === "timelineLane",
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
