import type { NodeAddress } from "./graphScope";

export type ObservationSource = {
    node: NodeAddress;
    kind: "output" | "stateField";
    member: string;
    channel?: number;
};
export type MeterMode = "level" | "value";
export type ObservationHint = { text: string; tooltip: string };
export type ObservationInterest = { id: string; source: ObservationSource; operation: string };
export type ObservationCapability = { nodeIndex: number; kind: ObservationSource["kind"]; member: string; operations: string[];
    channels?: Array<{ index: number; name: string }>;
    resultSchemas?: Array<{ operation: string; fields: Array<{ name: string; type: "float32" | "float64" | "uint64String" }> }> };
export type ObservationUpdate = {
    activationId: string; generation: string; revision: string; index: string; blockSize: number;
    captureNs?: number | null;
    scopes: Array<{ scopeId: string; values: Array<{ id: string; available: boolean; value?: Record<string, number | string | null> }> }>;
};
export type ObservationHostRequest =
    | { type: "observationDescribe"; requestId: string; nodes: NodeAddress[] }
    | { type: "observationReconcile"; requestId: string; interests: ObservationInterest[] };
export type ObservationResponse = { type: "observationResponse"; requestId: string; result?: unknown; error?: string };
export type ObservationDisplayValue = { activationId?: string; available: boolean; value?: Record<string, number | string | null> };

export function observationSourceKey(source: ObservationSource): string {
    return JSON.stringify([source.node.instanceId, source.node.path ?? [], source.kind, source.member, source.channel ?? null]);
}

// A host scope replaces its complete desired set. It never lets an older
// activation or a result from an earlier request revision replace current data.
export class ObservationValues {
    private activation = 0n;
    private revision = 0n;
    readonly values = new Map<string, ObservationDisplayValue>();
    acceptRevision(revision: string): void { this.revision = BigInt(revision) > this.revision ? BigInt(revision) : this.revision; }
    receive(update: ObservationUpdate, scopeId: string): boolean {
        const activation = BigInt(update.activationId);
        if (activation < this.activation || BigInt(update.revision) < this.revision) return false;
        const scope = update.scopes.find((scope) => scope.scopeId === scopeId);
        if (!scope) return false;
        this.activation = activation;
        for (const item of scope.values) this.values.set(item.id, { activationId: update.activationId, available: item.available, value: item.value });
        return true;
    }
    clear(): void { this.activation = 0n; this.revision = 0n; this.values.clear(); }
}
