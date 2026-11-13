import type { GraphScope, GraphScopeNode } from "./graphScope";

// Copy the latest host snapshot, not browser-supplied metadata. Include
// connections into this instance's interior with their exact selector paths.
export function nodeDebugDetails(scope: GraphScope, node: GraphScopeNode): string {
    return JSON.stringify({
        generation: scope.generation,
        ...node,
        connections: scope.connections.filter(({ connection }) => [...connection.inputs, ...connection.outputs]
            .some((matcher) => matcher.instanceId === node.address.instanceId)),
    }, null, 2);
}
