import type { ObservationUpdate, ObservationWindowResult } from "./observations";

type ObservationWindowBody = Pick<ObservationWindowResult, "regions" | "marks" | "unavailableRegions">;
type ObservationWindowHeader = Omit<ObservationWindowResult, keyof ObservationWindowBody | "id">;
type ObservationWindowPatch = { [Field in keyof ObservationWindowHeader]?: ObservationWindowHeader[Field] | null }
    & { body?: ObservationWindowBody | null };
type ObservationValue = Omit<ObservationUpdate["scopes"][number]["values"][number], "id">;
type ObservationScopePatch = {
    values?: Record<string, ObservationValue | null>;
    windows?: Record<string, ObservationWindowPatch | null>;
};
export type ObservationDelivery = Omit<ObservationUpdate, "scopes"> & {
    snapshot: boolean;
    scopes: Record<string, ObservationScopePatch | null>;
};

type Window = { header: ObservationWindowHeader; body?: ObservationWindowBody };
type Scope = { values: Map<string, ObservationValue>; windows: Map<string, Window> };

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

    read(scopeId: string): ObservationUpdate | undefined {
        if (!this.frame) return;
        const scope = this.scopes.get(scopeId);
        const values = Array.from(scope?.values ?? [], ([id, value]) => ({ id, ...value }));
        const windows = Array.from(scope?.windows ?? [], ([id, { header, body }]) =>
            ({ id, ...header, ...(body ?? { regions: [] }) }));
        return { ...this.frame, scopes: [{ scopeId, values, windows }] };
    }
}

export class ObservationDeliveryReceiver {
    private sequence?: number;
    private awaitingSnapshot = false;
    private readonly state = new ObservationDeliveryState();

    receive(update: ObservationDelivery, sequence: number, previousSequence?: number): "applied" | "ignored" | "resync" {
        if (this.sequence !== undefined && sequence <= this.sequence) return "ignored";
        if (!update.snapshot && (this.awaitingSnapshot || this.sequence === undefined || this.sequence !== previousSequence)) {
            if (this.awaitingSnapshot) return "ignored";
            this.awaitingSnapshot = true;
            return "resync";
        }
        this.state.receive(update);
        this.sequence = sequence;
        this.awaitingSnapshot = false;
        return "applied";
    }

    read(scopeId: string): ObservationUpdate | undefined { return this.state.read(scopeId); }
}
