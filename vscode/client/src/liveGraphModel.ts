import {
    VirtualContinuousPort,
    VirtualEventPort,
    VirtualNode,
    VirtualPortBinding,
} from "./graphModel";
import {
    SerializedLiveGraphInstance,
    SerializedLiveGraphGroup,
    SerializedLiveGraphNode,
    SerializedLiveGraphPort,
} from "./liveGraphProtocol";

function virtualIdentitySummary(node: VirtualNode): string {
    const identity = typeof node?.packageIdentity === "string" && node.packageIdentity.length > 0
        ? node.packageIdentity
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

type PortPresentation = VirtualContinuousPort | VirtualEventPort;

function bindingSummary(
    binding: VirtualPortBinding,
    connectivity: string,
): string {
    switch (binding) {
    case "interface":
        return "graph interface";
    case "virtualInput":
        return "follow virtual input";
    case "virtualOutput":
        return "virtual output";
    case "mixed":
        return "mixed built-in and virtual input";
    case "none":
        return connectivity === "connected" ? "built-in connection"
            : connectivity === "mixed" ? "mixed connections"
                : "disconnected";
    }
}

function continuousTypeSummary(port: VirtualContinuousPort): string {
    const types = port.valueTypes.map((value) => value.type).join(" | ");
    if (!port.channelType) return types;
    return types.length > 0 ? `${types} • ${port.channelType}` : port.channelType;
}

function portTypeSummary(port: PortPresentation): string {
    return "valueTypes" in port
        ? continuousTypeSummary(port)
        : port.eventType;
}

function serializePort(
    port: PortPresentation,
    index: number,
): SerializedLiveGraphPort {
    return {
        name: port.name || `[${index}]`,
        connectivity: port.connectivity,
        binding: port.binding,
        index: Number.isInteger(port.index) ? Number(port.index) : index,
        typeSummary: portTypeSummary(port),
        bindingSummary: bindingSummary(port.binding, port.connectivity),
    };
}

function makePortGroup(
    label: string,
    ports: PortPresentation[] | undefined,
    direction: "input" | "output",
    portKind: "continuous" | "event",
): SerializedLiveGraphGroup | null {
    if (!Array.isArray(ports) || ports.length === 0) {
        return null;
    }

    return {
        label,
        count: ports.length,
        direction,
        portKind,
        ports: ports.map((port, index) => serializePort(port, index)),
    };
}

export function serializeLiveGraphNodes(nodes: VirtualNode[]): SerializedLiveGraphNode[] {
    return nodes.map((node) => ({
        id: node.id || "",
        instanceId: node.instanceId || "",
        kind: node.kind || node.id || "",
        description: Number(node.memberCount || 0) > 1
            ? (() => {
                const identity = virtualIdentitySummary(node);
                return identity
                    ? `virtual • ${identity} • ${node.memberCount} concrete nodes`
                    : `virtual • ${node.memberCount} concrete nodes`;
            })()
            : `virtual • ${virtualIdentitySummary(node) || node.id || ""}`,
        tooltip: `virtual node • ${node.kind || "node"}${virtualIdentitySummary(node) ? ` • ${virtualIdentitySummary(node)}` : ""}${Number(node.memberCount || 0) > 1 ? ` • ${node.memberCount} concrete nodes` : ""}`,
        memberCount: Number(node.memberCount || 0),
        icon: Number(node.memberCount || 0) > 1 ? "merged" : "single",
        groups: [
            makePortGroup(
                "continuous inputs", node.continuousInputs,
                "input", "continuous"),
            makePortGroup(
                "continuous outputs", node.continuousOutputs,
                "output", "continuous"),
            makePortGroup("event inputs", node.eventInputs, "input", "event"),
            makePortGroup("event outputs", node.eventOutputs, "output", "event"),
        ].filter((group): group is SerializedLiveGraphGroup => group !== null),
        members: Array.isArray(node.members)
            ? node.members.map((member) => ({
                index: Number(member.index || 0),
                backingNodeId: member.backingNodeId || "",
                kind: member.kind || node.kind || "",
                description: `concrete ${Number(member.index || 0)}`,
                groups: [
                    makePortGroup(
                        "continuous inputs", member.continuousInputs,
                        "input", "continuous"),
                    makePortGroup(
                        "continuous outputs", member.continuousOutputs,
                        "output", "continuous"),
                    makePortGroup(
                        "event inputs", member.eventInputs,
                        "input", "event"),
                    makePortGroup(
                        "event outputs", member.eventOutputs,
                        "output", "event"),
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
    packageRoot?: string;
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
            packageRoot: instance.packageRoot || "",
            realized: Boolean(instance.realized),
            label: instance.displayName || (moduleId ? `${moduleId} • ${instanceId}` : instanceId),
        };
    });
}
