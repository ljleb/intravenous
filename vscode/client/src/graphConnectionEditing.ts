import { addressKey, matchingScopePorts, oppositePortDirection, resolveScopePorts, scopeNodeForAddress, type GraphPortMatcher, type GraphScope,
    type GraphScopeConnection, type GraphScopePort, type NodeAddress } from "./graphScope";
import { isGeneratedSnapConnection, isTimelineSnapAddress, timelineGridMode } from "./timelineSnap";

const defaultLoopExtraLatency = 1;

export type GraphPortEndpoint = {
    address: NodeAddress;
    kind: "sample" | "event";
    direction: "input" | "output";
    index: number;
    name: string;
    moduleBoundary?: boolean;
};

// Capture the semantic record at pickup. Re-read it before committing, so a
// gesture cannot overwrite an edit made in another view while it was in flight.
export type GraphConnectionEnd = {
    connection: GraphScopeConnection;
    port: GraphPortEndpoint;
};
export type GraphConnectionEdit = { end: GraphConnectionEnd; target: GraphPortEndpoint | null; detach?: true };
export type GraphConnectionChange = GraphConnectionEdit
    | { end: GraphConnectionEnd; toggleDetach: true } | { end: GraphConnectionEnd; loopExtraLatency: number };
export type GraphConnectionMutation = { type: "delete"; id: string }
    | { type: "upsert"; connection: Record<string, unknown> };

function resolveEndpoint(scope: GraphScope, endpoint: GraphPortEndpoint) {
    const node = scope.nodes.find((entry) => addressKey(entry.address) === addressKey(endpoint.address));
    if (!node) throw new Error("The selected node is no longer available in this scope");
    const [match] = matchingScopePorts(node, { ...matcherFor(endpoint),
        port: { index: endpoint.index, name: endpoint.name } }, endpoint.kind, endpoint.direction);
    const port = match?.port;
    if (!port) throw new Error("The selected port changed; select it again");
    return port;
}
function portType(port: GraphScopePort, kind: GraphPortEndpoint["kind"]): string {
    const type = kind === "sample" ? port.channelType : port.eventType;
    if (!type) throw new Error("The selected port type is unavailable");
    return type;
}
function matcherFor(endpoint: GraphPortEndpoint, channel?: number): GraphPortMatcher {
    return { ...endpoint.address, path: endpoint.address.path ?? [],
        ...(endpoint.moduleBoundary ? { moduleBoundary: true } : {}),
        port: { ...(endpoint.name ? { name: endpoint.name } : {}), index: endpoint.index,
            ...(channel === undefined ? {} : { channel }) } };
}
export function sameGraphPort(first: GraphPortEndpoint, second: GraphPortEndpoint): boolean {
    return addressKey(first.address) === addressKey(second.address) && first.kind === second.kind
        && !!first.moduleBoundary === !!second.moduleBoundary
        && first.direction === second.direction && first.index === second.index && first.name === second.name;
}
function matchesPort(scope: GraphScope, matcher: GraphPortMatcher, port: GraphPortEndpoint): boolean {
    const node = scopeNodeForAddress(scope, matcher);
    return !!node && addressKey(node.address) === addressKey(port.address)
        && !!matcher.moduleBoundary === !!port.moduleBoundary
        && (matcher.port.name === undefined || matcher.port.name === port.name)
        && (matcher.port.index === undefined || matcher.port.index === port.index);
}
export function connectionEndsAtPort(scope: GraphScope, port: GraphPortEndpoint): GraphConnectionEnd[] {
    return scope.connections.filter((entry) => !entry.sourceOwned && !isGeneratedSnapConnection(entry) && entry.connection.kind === port.kind
        && (port.direction === "input" ? entry.connection.inputs : entry.connection.outputs)
            .some((matcher) => matchesPort(scope, matcher, port)))
        .map((connection) => ({ connection, port }));
}
export function fixedConnectionPorts(scope: GraphScope, end: GraphConnectionEnd): GraphPortEndpoint[] {
    const connection = end.connection.connection;
    const direction = oppositePortDirection(end.port.direction);
    const matchers = direction === "output" ? connection.outputs : connection.inputs;
    return matchers.flatMap((matcher) => resolveScopePorts(scope, matcher, connection.kind, direction)
        .map(({ node, port }): GraphPortEndpoint => ({ address: node.address, kind: connection.kind,
            direction, index: port.index, name: port.name, ...(matcher.moduleBoundary ? { moduleBoundary: true } : {}) })));
}

function connectionKey(entry: GraphScopeConnection): string {
    const matchers = (values: GraphPortMatcher[]) => values.map((value) => [addressKey(value),
        !!value.moduleBoundary, value.port.name ?? null, value.port.index ?? null, value.port.channel ?? null]);
    const c = entry.connection;
    return JSON.stringify([entry.id, c.kind, c.sourceType, c.targetType, c.loopExtraLatency, matchers(c.outputs), matchers(c.inputs)]);
}

export function canDetachConnection(connection: GraphScopeConnection["connection"]): boolean {
    return !connection.inputs.some(isTimelineSnapAddress);
}

export class ConnectionDelayEdits {
    private end: GraphConnectionEnd | undefined;
    private next: number | undefined;
    private sending = false;

    constructor(private readonly write: (end: GraphConnectionEnd, samples: number) => Promise<boolean>,
        private readonly changed: (pending: boolean) => void, private readonly rejected: () => void) {}

    get pending(): boolean { return this.sending; }

    begin(end: GraphConnectionEnd): boolean {
        if (this.sending || end.connection.connection.loopExtraLatency === undefined) return false;
        this.end = end;
        return true;
    }

    update(samples: number): void {
        if (!this.end) return;
        this.next = samples;
        if (!this.sending) void this.flush();
    }

    discard(): void {
        this.next = undefined;
        this.end = undefined;
    }

    private async flush(): Promise<void> {
        this.sending = true;
        this.changed(true);
        try {
            while (this.end && this.next !== undefined) {
                const end: GraphConnectionEnd = this.end;
                const samples = this.next;
                this.next = undefined;
                const { connection } = end;
                if (samples === connection.connection.loopExtraLatency) continue;
                if (!await this.write(end, samples)) throw new Error("Feedback delay edit was rejected");
                if (this.end === end) this.end = { ...end, connection: { ...connection,
                    connection: { ...connection.connection, loopExtraLatency: samples } } };
            }
        } catch {
            this.discard();
            this.rejected();
        } finally {
            this.sending = false;
            this.changed(false);
        }
    }
}

export function connectionEndMutation(scope: GraphScope, edit: GraphConnectionChange): GraphConnectionMutation | null {
    if (!scope.editable) throw new Error("This graph is read-only");
    const { end } = edit;
    const current = scope.connections.find((entry) => entry.id === end.connection.id);
    if (!current || connectionKey(current) !== connectionKey(end.connection)) {
        throw new Error("The selected connection changed; select it again");
    }
    if (current.sourceOwned) throw new Error("Edit this connection in the module source");
    if (isGeneratedSnapConnection(current)) throw new Error("The ruler manages this connection");
    const original = resolveEndpoint(scope, end.port);
    const connection = current.connection;
    if (connection.kind !== end.port.kind) throw new Error("The selected connection changed; select it again");
    if ("toggleDetach" in edit || "loopExtraLatency" in edit) {
        const matchers = end.port.direction === "input" ? connection.inputs : connection.outputs;
        if (!matchers.some((matcher) => matchesPort(scope, matcher, end.port)))
            throw new Error("The selected connection no longer reaches this port");
        if (!canDetachConnection(connection)) throw new Error("The ruler Beats input cannot use a detached connection");
        const { loopExtraLatency, ...record } = connection;
        if ("loopExtraLatency" in edit) {
            if (loopExtraLatency === undefined) throw new Error("Detach the connection before changing its feedback delay");
            if (!Number.isSafeInteger(edit.loopExtraLatency) || edit.loopExtraLatency < 1)
                throw new Error("The feedback delay must be a positive whole number of samples");
            if (loopExtraLatency === edit.loopExtraLatency) return null;
            return { type: "upsert", connection: { ...record, connectionId: current.id, loopExtraLatency: edit.loopExtraLatency } };
        }
        return { type: "upsert", connection: { ...record, connectionId: current.id,
            ...(loopExtraLatency === undefined && { loopExtraLatency: defaultLoopExtraLatency }) } };
    }
    const { target, detach } = edit;
    if (target && (target.kind !== end.port.kind || target.direction !== end.port.direction))
        throw new Error("Move the connection end to a port of the same kind and direction");
    if (target && sameGraphPort(end.port, target)) return null;
    const side = end.port.direction === "input" ? "inputs" : "outputs";
    const typeField = end.port.direction === "input" ? "targetType" : "sourceType";
    const matchers: GraphPortMatcher[] = [];
    let found = false;
    // A selector can match several visible ports. Expand only the selector being
    // edited, preserving its channel selection and the order of its other ports.
    for (const matcher of connection[side]) {
        if (!matchesPort(scope, matcher, end.port)) { matchers.push(matcher); continue; }
        const resolved = resolveScopePorts(scope, matcher, connection.kind, end.port.direction);
        for (const { node, port } of resolved) {
            const endpoint = { address: node.address, kind: connection.kind, direction: end.port.direction,
                index: port.index, name: port.name, moduleBoundary: matcher.moduleBoundary };
            if (!sameGraphPort(endpoint, end.port)) {
                matchers.push(matcherFor(endpoint, matcher.port.channel));
            } else {
                found = true;
                if (target) matchers.push(matcherFor(target, matcher.port.channel));
            }
        }
    }
    if (!found) throw new Error("The selected connection no longer reaches this port");
    if (!matchers.length) return { type: "delete", id: current.id };
    let type = connection[typeField];
    if (!type || !connection.sourceType || !connection.targetType) throw new Error("The selected connection type is unavailable");
    if (target) {
        const originalType = portType(original, end.port.kind);
        const targetType = portType(resolveEndpoint(scope, target), target.kind);
        const otherEnds = connection[side].some((matcher) => !matchesPort(scope, matcher, end.port)
            || resolveScopePorts(scope, matcher, connection.kind, end.port.direction)
                .some(({ node, port }) => !sameGraphPort({ address: node.address, kind: connection.kind,
                    direction: end.port.direction, index: port.index, name: port.name, moduleBoundary: matcher.moduleBoundary }, end.port)));
        const selectedChannels = connection[side].some((matcher) => matchesPort(scope, matcher, end.port)
            && matcher.port.channel !== undefined);
        if (targetType !== originalType && (otherEnds || selectedChannels)) {
            throw new Error("This connection shares a port type or selects individual channels; choose a port of the same type");
        }
        if (!otherEnds && !selectedChannels) type = targetType;
        if (target.direction === "input" && isTimelineSnapAddress(target.address)) {
            requireCustomBeatInput(scope, connection.sourceType);
        } else if (target.direction === "output" && connection.inputs.some(isTimelineSnapAddress)) {
            requireCustomBeatInput(scope, type);
        }
    }
    const updated = { ...connection, connectionId: current.id, [side]: matchers, [typeField]: type,
        ...(detach && { loopExtraLatency: connection.loopExtraLatency ?? defaultLoopExtraLatency }) };
    if (updated.loopExtraLatency !== undefined && !canDetachConnection(updated))
        throw new Error("The ruler Beats input cannot use a detached connection");
    return { type: "upsert", connection: updated };
}

export function canMoveConnectionEnd(scope: GraphScope, end: GraphConnectionEnd, target: GraphPortEndpoint): boolean {
    try { connectionEndMutation(scope, { end, target }); return true; }
    catch { return false; }
}

export function canContinuePortConnection(scope: GraphScope | null, port: GraphPortEndpoint, end?: GraphConnectionEnd): boolean {
    if (!scope?.editable) return false;
    if (isTimelineSnapAddress(port.address) && timelineGridMode(scope) !== "custom") return false;
    // Moving back to the pickup port is a no-op after checking the captured connection.
    if (end) return canMoveConnectionEnd(scope, end, port);
    try { portType(resolveEndpoint(scope, port), port.kind); return true; }
    catch { return false; }
}

export function connectionFromPorts(scope: GraphScope, id: string,
    first: GraphPortEndpoint, second: GraphPortEndpoint, detach = false): GraphScopeConnection["connection"] {
    if (!scope.editable) throw new Error("This graph is read-only");
    if (first.kind !== second.kind || first.direction === second.direction) {
        throw new Error("Connect an input and an output of the same port kind");
    }
    const output = first.direction === "output" ? first : second;
    const input = first.direction === "input" ? first : second;
    const sourceType = portType(resolveEndpoint(scope, output), output.kind);
    const targetType = portType(resolveEndpoint(scope, input), input.kind);
    if (isTimelineSnapAddress(input.address)) {
        if (detach) throw new Error("The ruler Beats input cannot use a detached connection");
        requireCustomBeatInput(scope, sourceType);
    }
    return { connectionId: id, kind: output.kind, sourceType, targetType,
        outputs: [matcherFor(output)], inputs: [matcherFor(input)], ...(detach && { loopExtraLatency: defaultLoopExtraLatency }) };
}

function requireCustomBeatInput(scope: GraphScope, sourceType: string): void {
    if (timelineGridMode(scope) !== "custom" || sourceType !== "beat") {
        throw new Error("The ruler Beats input accepts Beat Event outputs in Custom snap mode");
    }
}
