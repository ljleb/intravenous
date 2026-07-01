import { LogicalNode, LogicalNodeMember, LogicalPort } from "./graphModel";
import {
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

function inputStateActions(memberOrdinal: number | null, family: "sampleInput" | "eventInput"): LiveGraphPortStateAction[] {
    return [
        { state: "default", label: "Reset to default", reset: true },
        ...(memberOrdinal != null ? [{ state: "logicalFollow", label: "Follow logical value" }] : []),
        { state: "disconnected", label: "Disconnect" },
        { state: "timelineLane", label: "Drive from lane" },
    ];
}

function outputStateActions(memberOrdinal: number | null): LiveGraphPortStateAction[] {
    return [
        { state: "disconnected", label: "Reset to disconnected", reset: true },
        ...(memberOrdinal != null ? [{ state: "logical", label: "Follow logical output" }] : []),
        { state: "timelineLane", label: "Drive to lane" },
    ];
}

function describePortState(
    port: LogicalPort,
    family: LiveGraphPortStateFamily | null,
    memberOrdinal: number | null,
): { summary: string; resetState: string | null; actions: LiveGraphPortStateAction[] } {
    const connectivity = typeof port.connectivity === "string" ? port.connectivity : "disconnected";

    if (family === "sampleInput") {
        if (memberOrdinal != null) {
            return {
                summary: port.hasConcreteOverride ? "override" : "default",
                resetState: "default",
                actions: inputStateActions(memberOrdinal, family),
            };
        }
        return {
            summary: connectivity,
            resetState: "default",
            actions: inputStateActions(memberOrdinal, family),
        };
    }

    if (family === "eventInput") {
        return {
            summary: connectivity,
            resetState: "default",
            actions: inputStateActions(memberOrdinal, family),
        };
    }

    if (family === "sampleOutput" || family === "eventOutput") {
        return {
            summary: connectivity,
            resetState: "disconnected",
            actions: outputStateActions(memberOrdinal),
        };
    }

    return {
        summary: connectivity,
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
    forceTweakable = false,
): SerializedLiveGraphPort {
    const stateFamily: LiveGraphPortStateFamily | null =
        direction === "input" && portKind === "sample" ? "sampleInput"
            : direction === "input" && portKind === "event" ? "eventInput"
                : direction === "output" && portKind === "sample" ? "sampleOutput"
                    : direction === "output" && portKind === "event" ? "eventOutput"
                        : null;
    const connectivity = typeof port.connectivity === "string" ? port.connectivity : "disconnected";
    const tweakable = (forceTweakable || direction === "input")
        && direction === "input"
        && portKind === "sample"
        && (connectivity === "mixed" || connectivity === "disconnected");
    const state = describePortState(port, stateFamily, memberOrdinal);

    return {
        name: port.name || `[${index}]`,
        connectivity,
        ordinal: Number.isInteger(port.ordinal) ? Number(port.ordinal) : index,
        defaultValue: typeof port.defaultValue === "number" ? port.defaultValue : 0,
        currentValue: typeof port.currentValue === "number" ? port.currentValue : 0,
        hasConcreteOverride: Boolean(port.hasConcreteOverride),
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
            forceTweakable,
        )),
    };
}

export function serializeLiveGraphNodes(nodes: LogicalNode[]): SerializedLiveGraphNode[] {
    return nodes.map((node) => ({
        id: node.id || "",
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
            makePortGroup("sample inputs", node.sampleInputs, "input", "sample", null),
            makePortGroup("sample outputs", node.sampleOutputs, "output", "sample", null),
            makePortGroup("event inputs", node.eventInputs, "input", "event", null),
            makePortGroup("event outputs", node.eventOutputs, "output", "event", null),
        ].filter((group): group is SerializedLiveGraphGroup => group !== null),
        members: Array.isArray(node.members)
            ? node.members.map((member) => ({
                ordinal: Number(member.ordinal || 0),
                backingNodeId: member.backingNodeId || "",
                kind: member.kind || node.kind || "",
                description: `member ${Number(member.ordinal || 0)}`,
                groups: [
                    makePortGroup("sample inputs", sampleInputsForMemberView(node, member), "input", "sample", Number(member.ordinal || 0), true),
                    makePortGroup("sample outputs", member.sampleOutputs, "output", "sample", Number(member.ordinal || 0)),
                    makePortGroup("event inputs", member.eventInputs, "input", "event", Number(member.ordinal || 0)),
                    makePortGroup("event outputs", member.eventOutputs, "output", "event", Number(member.ordinal || 0)),
                ].filter((group): group is SerializedLiveGraphGroup => group !== null),
            }))
            : [],
    }));
}

export function applySampleInputValueUpdate(
    nodes: LogicalNode[],
    nodeId: string,
    ordinal: number,
    value: unknown,
    memberOrdinal: number | null = null,
): void {
    for (const node of nodes) {
        if (node.id !== nodeId) {
            continue;
        }
        const updateInputs = (inputs: LogicalPort[] | undefined, markOverride: boolean | null) => {
            if (!Array.isArray(inputs)) {
                return;
            }
            for (const input of inputs) {
                if (Number(input.ordinal) === Number(ordinal)) {
                    input.currentValue = typeof value === "number" ? value : Number(value);
                    if (markOverride != null) {
                        input.hasConcreteOverride = markOverride;
                    }
                }
            }
        };
        const inputs = memberOrdinal == null
            ? node.sampleInputs
            : ((node.members || []).find((member) => Number(member.ordinal) === Number(memberOrdinal)) || {}).sampleInputs;
        updateInputs(inputs, memberOrdinal == null ? null : true);
        if (memberOrdinal == null) {
            for (const member of node.members || []) {
                const memberInputs = Array.isArray(member.sampleInputs) ? member.sampleInputs : [];
                for (const input of memberInputs) {
                    if (Number(input.ordinal) === Number(ordinal) && !input.hasConcreteOverride) {
                        input.currentValue = typeof value === "number" ? value : Number(value);
                    }
                }
            }
        }
    }
}

export function clearSampleInputValueOverride(
    nodes: LogicalNode[],
    nodeId: string,
    memberOrdinal: number,
    ordinal: number,
): void {
    for (const node of nodes) {
        if (node.id !== nodeId) {
            continue;
        }
        const logicalInput = Array.isArray(node.sampleInputs)
            ? node.sampleInputs.find((input) => Number(input.ordinal) === Number(ordinal))
            : null;
        const logicalValue = logicalInput && typeof logicalInput.currentValue === "number"
            ? logicalInput.currentValue
            : null;
        const member = (node.members || []).find((candidate) => Number(candidate.ordinal) === Number(memberOrdinal));
        const inputs = member && Array.isArray(member.sampleInputs) ? member.sampleInputs : [];
        for (const input of inputs) {
            if (Number(input.ordinal) === Number(ordinal)) {
                input.hasConcreteOverride = false;
                if (logicalValue != null) {
                    input.currentValue = logicalValue;
                }
            }
        }
    }
}
