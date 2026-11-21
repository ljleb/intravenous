import type { ObservationDelivery, ObservationValue } from "../src/observationDelivery";
import type { ObservationValues, ObservationWindowResult } from "../src/observations";

export type ObservationSnapshotFixture = Omit<ObservationDelivery, "snapshot" | "scopes"> & {
    scopes: Array<{ scopeId: string; values: Array<{ id: string } & ObservationValue>; windows?: ObservationWindowResult[] }>;
};

export function observationSnapshot({ scopes, ...frame }: ObservationSnapshotFixture): ObservationDelivery {
    return { ...frame, snapshot: true, scopes: Object.fromEntries(scopes.map(({ scopeId, values, windows }) => [scopeId, {
        values: Object.fromEntries(values.map(({ id, ...value }) => [id, value])),
        windows: Object.fromEntries((windows ?? []).map(({ id, regions, marks, unavailableRegions, ...header }) =>
            [id, { ...header, body: { regions, marks, unavailableRegions } }])),
    }])) };
}

export function receiveSnapshot(values: ObservationValues, frame: ObservationSnapshotFixture): boolean {
    return values.receive(observationSnapshot(frame));
}
