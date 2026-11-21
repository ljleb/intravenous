import type { ObservationDisplayValue, ObservationWindowResult } from "./observations";

type ObservationWindowBody = Pick<ObservationWindowResult, "regions" | "marks" | "unavailableRegions">;
type ObservationWindowHeader = Omit<ObservationWindowResult, keyof ObservationWindowBody | "id">;
type ObservationWindowPatch = { [Field in keyof ObservationWindowHeader]?: ObservationWindowHeader[Field] | null }
    & { body?: ObservationWindowBody | null };
export type ObservationValue = Omit<ObservationDisplayValue, "activationId">;
type ObservationScopePatch = {
    values?: Record<string, ObservationValue | null>;
    windows?: Record<string, ObservationWindowPatch | null>;
};
export type ObservationDelivery = {
    activationId: string; generation: string; revision: string; index: string; blockSize: number; dataVersion: string;
    captureNs?: number | null;
    snapshot: boolean;
    scopes: Record<string, ObservationScopePatch | null>;
};

type Window = { header: ObservationWindowHeader; body?: ObservationWindowBody };
type Scope = { values: Map<string, ObservationValue>; windows: Map<string, Window> };
const emptyWindowBody: ObservationWindowBody = { regions: [] };
function windowResult(id: string, { header, body }: Window): ObservationWindowResult {
    return { id, ...header, ...(body ?? emptyWindowBody) };
}

export type ObservationScopeChanges = {
    snapshot: boolean;
    values: Array<[string, ObservationValue | null]>;
    windows: Array<[string, ObservationWindowResult | null]>;
};

export class ObservationDeliveryState {
    private frame?: Omit<ObservationDelivery, "snapshot" | "scopes">;
    private readonly scopes = new Map<string, Scope>();

    clear(): void {
        this.frame = undefined;
        this.scopes.clear();
    }

    receive({ snapshot, scopes, ...frame }: ObservationDelivery): void {
        if (snapshot) this.scopes.clear();
        this.frame = frame;
        for (const [id, changes] of Object.entries(scopes)) {
            if (!changes) {
                this.scopes.delete(id);
                continue;
            }
            let scope = this.scopes.get(id);
            if (!scope) this.scopes.set(id, scope = { values: new Map(), windows: new Map() });
            for (const [key, value] of Object.entries(changes.values ?? {})) {
                if (value) scope.values.set(key, value);
                else scope.values.delete(key);
            }
            for (const [key, change] of Object.entries(changes.windows ?? {})) {
                if (!change) {
                    scope.windows.delete(key);
                    continue;
                }
                const previous = scope.windows.get(key);
                const { body, ...fields } = change;
                const header = { ...previous?.header, ...fields };
                for (const field of Object.keys(fields) as (keyof ObservationWindowHeader)[])
                    if (fields[field] === null) delete header[field];
                if (header.window && header.status && header.approximate !== undefined && header.approximate !== null) {
                    scope.windows.set(key, { header: header as ObservationWindowHeader,
                        body: body === undefined ? previous?.body : body ?? undefined });
                }
            }
        }
    }

    snapshot(scopeId: string, outgoingId: string): ObservationDelivery | undefined {
        if (!this.frame) return;
        const scope = this.scopes.get(scopeId);
        return { ...this.frame, snapshot: true, scopes: {
            [outgoingId]: scope ? {
                values: Object.fromEntries(scope.values),
                windows: Object.fromEntries(Array.from(scope.windows, ([id, { header, body }]) => [id, { ...header, body }])),
            } : {},
        } };
    }

    changes(scopeId: string, update?: ObservationDelivery): ObservationScopeChanges {
        const scope = this.scopes.get(scopeId);
        const changes = update?.scopes[scopeId];
        const snapshot = !update || update.snapshot || changes === null;
        const entries = <T>(current: Map<string, T> | undefined, patch: Record<string, unknown> | undefined): Array<[string, T | null]> =>
            snapshot ? [...current ?? []] : Object.keys(patch ?? {}).map((id) => [id, current?.get(id) ?? null]);
        return { snapshot, values: entries(scope?.values, changes?.values),
            windows: entries(scope?.windows, changes?.windows).map(([id, window]) => [id,
                window ? windowResult(id, window) : null]) };
    }

    window(scopeId: string, id: string): ObservationWindowResult | undefined {
        const window = this.scopes.get(scopeId)?.windows.get(id);
        return window && windowResult(id, window);
    }
}

export class ObservationDeliverySequence {
    private sequence?: number;
    private awaitingSnapshot = false;

    receive(update: ObservationDelivery, sequence: number, previousSequence?: number): "applied" | "ignored" | "resync" {
        if (this.sequence !== undefined && sequence <= this.sequence) return "ignored";
        if (!update.snapshot && (this.awaitingSnapshot || this.sequence === undefined || this.sequence !== previousSequence)) {
            if (this.awaitingSnapshot) return "ignored";
            this.awaitingSnapshot = true;
            return "resync";
        }
        this.sequence = sequence;
        this.awaitingSnapshot = false;
        return "applied";
    }
}
