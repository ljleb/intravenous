import { addressKey, resolveScopePorts, type GraphPortMatcher, type GraphScope,
    type GraphScopeConnection, type NodeAddress } from "./graphScope";

export type GraphPortEndpoint = {
    address: NodeAddress;
    kind: "sample" | "event";
    direction: "input" | "output";
    index: number;
    name: string;
};

// Capture the semantic record at pickup. Re-read it before committing, so a
// gesture cannot overwrite an edit made in another view while it was in flight.
export type GraphConnectionEnd = {
    connection: GraphScopeConnection;
    port: GraphPortEndpoint;
};
export type GraphConnectionEdit = { end: GraphConnectionEnd; target: GraphPortEndpoint | null };
export type GraphConnectionMutation = { type: "delete"; id: string }
    | { type: "upsert"; connection: Record<string, unknown> };

function resolveEndpoint(scope: GraphScope, endpoint: GraphPortEndpoint) {
    const node = scope.nodes.find((entry) => addressKey(entry.address) === addressKey(endpoint.address));
    if (!node || endpoint.address.path?.length) throw new Error("The selected node is no longer available in this scope");
    const ports = endpoint.kind === "sample"
        ? endpoint.direction === "input" ? node.sampleInputs : node.sampleOutputs
        : endpoint.direction === "input" ? node.eventInputs : node.eventOutputs;
    const port = ports.find((entry) => entry.index === endpoint.index && entry.name === endpoint.name);
    if (!port) throw new Error("The selected port changed; select it again");
    return port;
}
function endpointType(scope: GraphScope, endpoint: GraphPortEndpoint): string {
    const port = resolveEndpoint(scope, endpoint);
    const type = endpoint.kind === "sample" ? port.channelType : port.eventType;
    if (!type) throw new Error("The selected port type is unavailable");
    return type;
}
function matcherFor(endpoint: GraphPortEndpoint): GraphPortMatcher {
    return { ...endpoint.address, path: endpoint.address.path ?? [], port: { name: endpoint.name, index: endpoint.index } };
}
export function sameGraphPort(first: GraphPortEndpoint, second: GraphPortEndpoint): boolean {
    return addressKey(first.address) === addressKey(second.address) && first.kind === second.kind
        && first.direction === second.direction && first.index === second.index && first.name === second.name;
}
function matchesPort(matcher: GraphPortMatcher, port: GraphPortEndpoint): boolean {
    return addressKey(matcher) === addressKey(port.address)
        && (matcher.port.name === undefined || matcher.port.name === port.name)
        && (matcher.port.index === undefined || matcher.port.index === port.index);
}
export function connectionEndsAtPort(scope: GraphScope, port: GraphPortEndpoint): GraphConnectionEnd[] {
    return scope.connections.filter(({ connection }) => connection.kind === port.kind
        && (port.direction === "input" ? connection.inputs : connection.outputs)
            .some((matcher) => matchesPort(matcher, port)))
        .map((connection) => ({ connection, port }));
}
export function fixedConnectionPorts(scope: GraphScope, end: GraphConnectionEnd): GraphPortEndpoint[] {
    const connection = end.connection.connection;
    const direction = end.port.direction === "input" ? "output" : "input";
    const matchers = direction === "output" ? connection.outputs : connection.inputs;
    return matchers.flatMap((matcher) => resolveScopePorts(scope, matcher, connection.kind, direction)
        .map(({ node, port }): GraphPortEndpoint => ({ address: node.address, kind: connection.kind, direction, index: port.index, name: port.name })));
}

function connectionKey(entry: GraphScopeConnection): string {
    const matchers = (values: GraphPortMatcher[]) => values.map((value) => [addressKey(value),
        value.port.name ?? null, value.port.index ?? null, value.port.channel ?? null]);
    const c = entry.connection;
    return JSON.stringify([entry.id, c.kind, c.sourceType, c.targetType, matchers(c.outputs), matchers(c.inputs)]);
}

export function connectionEndMutation(scope: GraphScope, edit: GraphConnectionEdit): GraphConnectionMutation | null {
    if (!scope.editable) throw new Error("This graph is read-only");
    const { end, target } = edit;
    const current = scope.connections.find((entry) => entry.id === end.connection.id);
    if (!current || connectionKey(current) !== connectionKey(end.connection)) {
        throw new Error("The selected connection changed; select it again");
    }
    const originalType = endpointType(scope, end.port);
    const connection = current.connection;
    if (connection.kind !== end.port.kind) throw new Error("The selected connection changed; select it again");
    if (target) {
        if (target.kind !== end.port.kind || target.direction !== end.port.direction) {
            throw new Error("Move the connection end to a port of the same kind and direction");
        }
        resolveEndpoint(scope, target);
        if (sameGraphPort(end.port, target)) return null;
    }
    const side = end.port.direction === "input" ? "inputs" : "outputs";
    const typeField = end.port.direction === "input" ? "targetType" : "sourceType";
    const matchers: GraphPortMatcher[] = [];
    let found = false;
    // A selector can match several visible ports. Expand only the selector being
    // edited, preserving its channel selection and the order of its other ports.
    for (const matcher of connection[side]) {
        if (!matchesPort(matcher, end.port)) { matchers.push(matcher); continue; }
        const resolved = resolveScopePorts(scope, matcher, connection.kind, end.port.direction);
        for (const { node, port } of resolved) {
            const endpoint = { address: node.address, kind: connection.kind, direction: end.port.direction,
                index: port.index, name: port.name };
            if (!sameGraphPort(endpoint, end.port)) {
                matchers.push({ ...matcherFor(endpoint), port: { ...matcher.port, name: port.name, index: port.index } });
            } else {
                found = true;
                if (target) matchers.push({ ...matcherFor(target), port: { ...matcher.port, name: target.name, index: target.index } });
            }
        }
    }
    if (!found) throw new Error("The selected connection no longer reaches this port");
    if (!matchers.length) return { type: "delete", id: current.id };
    let type = connection[typeField];
    if (!type || !connection.sourceType || !connection.targetType) throw new Error("The selected connection type is unavailable");
    if (target) {
        const targetType = endpointType(scope, target);
        const otherEnds = connection[side].some((matcher) => !matchesPort(matcher, end.port)
            || resolveScopePorts(scope, matcher, connection.kind, end.port.direction)
                .some(({ node, port }) => !sameGraphPort({ address: node.address, kind: connection.kind,
                    direction: end.port.direction, index: port.index, name: port.name }, end.port)));
        const selectedChannels = connection[side].some((matcher) => matchesPort(matcher, end.port)
            && matcher.port.channel !== undefined);
        if (targetType !== originalType && (otherEnds || selectedChannels)) {
            throw new Error("This connection shares a port type or selects individual channels; choose a port of the same type");
        }
        if (!otherEnds && !selectedChannels) type = targetType;
    }
    return { type: "upsert", connection: { ...connection, connectionId: current.id, [side]: matchers, [typeField]: type } };
}

export function canMoveConnectionEnd(scope: GraphScope, end: GraphConnectionEnd, target: GraphPortEndpoint): boolean {
    try { connectionEndMutation(scope, { end, target }); return true; }
    catch { return false; }
}

export function connectionFromPorts(scope: GraphScope, id: string,
    first: GraphPortEndpoint, second: GraphPortEndpoint): Record<string, unknown> {
    if (!scope.editable) throw new Error("This graph is read-only");
    if (first.kind !== second.kind || first.direction === second.direction) {
        throw new Error("Connect an input and an output of the same port kind");
    }
    const output = first.direction === "output" ? first : second;
    const input = first.direction === "input" ? first : second;
    const sourceType = endpointType(scope, output);
    const targetType = endpointType(scope, input);
    return { connectionId: id, kind: output.kind, sourceType, targetType,
        outputs: [matcherFor(output)], inputs: [matcherFor(input)] };
}
