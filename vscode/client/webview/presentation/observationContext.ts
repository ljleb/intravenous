import { inject, provide, ref, type InjectionKey } from "vue";
import type { NodeAddress } from "../../src/graphScope";
import { addressKey } from "../../src/graphScope";
import { ObservationValues, observationSourceKey, type ObservationCapability, type ObservationHostRequest,
    type ObservationInterest, type ObservationResponse, type ObservationSource, type ObservationUpdate, type MeterMode, type ObservationHint } from "../../src/observations";

type NodeInterest = { node: NodeAddress; visible: boolean };
export type PresentedObservation = ObservationCapability & { source: ObservationSource; id: string; operation: string; channelName?: string };
type Capability = PresentedObservation;
export class PresentationObservations {
    readonly version = ref(0);
    readonly capabilities = ref<Capability[]>([]);
    readonly values = new ObservationValues();
    readonly error = ref("");
    private nodes = new Map<symbol, NodeInterest>();
    private visible = true;
    private pending = new Map<string, { resolve(value: unknown): void; reject(error: Error): void; timer: ReturnType<typeof setTimeout> }>();
    private timer: ReturnType<typeof setTimeout> | undefined;
    private heartbeat: ReturnType<typeof setInterval>;
    private sequence = 0;
    private sourceIds = new Map<string, string>();
    private nextSourceId = 0;
    private epoch = 0;
    private disposed = false;
    private modes = new Map<string, MeterMode>();
    private hints = new Map<symbol, ObservationHint>();
    private desired: ObservationInterest[] = [];
    private queue = Promise.resolve();
    constructor(private readonly send: (request: ObservationHostRequest) => void,
        private readonly publishHint: (hint: ObservationHint | null) => void = () => {},
        private readonly preferences?: { initial?: Record<string, MeterMode>; save(modes: Record<string, MeterMode>): void }) {
        for (const [key, mode] of Object.entries(preferences?.initial ?? {}).slice(0, 4096))
            if (mode === "level" || mode === "value") this.modes.set(key, mode);
        this.heartbeat = setInterval(() => { if (this.desired.length) void this.reconcile(this.desired); }, 10000);
    }
    private request(value: Omit<Extract<ObservationHostRequest, { type: "observationDescribe" }>, "requestId">
        | Omit<Extract<ObservationHostRequest, { type: "observationReconcile" }>, "requestId">): Promise<unknown> {
        const requestId = `observation:${++this.sequence}`;
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this.pending.delete(requestId);
                reject(new Error("Observation request timed out"));
            }, 30000);
            this.pending.set(requestId, { resolve, reject, timer });
            try {
                // Node addresses come from Vue's reactive graph state. Only
                // plain JSON data can cross the webview's postMessage boundary.
                const message = JSON.parse(JSON.stringify({ ...value, requestId })) as ObservationHostRequest;
                this.send(message);
            } catch (error) {
                clearTimeout(timer);
                this.pending.delete(requestId);
                reject(error);
            }
        });
    }
    receive(message: ObservationResponse | { type: "observationUpdate"; update: ObservationUpdate }): void {
        if (message.type === "observationUpdate") {
            if (this.values.receive(message.update, "viewport")) ++this.version.value;
            return;
        }
        const pending = this.pending.get(message.requestId);
        if (!pending) return;
        this.pending.delete(message.requestId);
        clearTimeout(pending.timer);
        if (message.error) pending.reject(new Error(message.error));
        else pending.resolve(message.result);
    }
    node(owner: symbol, node: NodeAddress, visible: boolean): void {
        const previous = this.nodes.get(owner);
        if (previous?.visible === visible && addressKey(previous.node) === addressKey(node)) return;
        this.nodes.set(owner, { node, visible });
        this.schedule();
    }
    remove(owner: symbol): void { this.nodes.delete(owner); this.schedule(); }
    setVisible(visible: boolean): void {
        if (this.visible === visible) return;
        this.visible = visible;
        this.schedule();
    }
    refresh(): void { this.schedule(); }
    private schedule(): void {
        ++this.epoch;
        if (this.timer) clearTimeout(this.timer);
        // A short grace period avoids compiling a new probe plan on every
        // scroll frame. The host replaces one full viewport interest set.
        this.timer = setTimeout(() => { this.timer = undefined; void this.discover(); }, 200);
    }
    private async discover(): Promise<void> {
        const epoch = this.epoch;
        const unique = new Map<string, NodeAddress>();
        if (this.visible) for (const entry of this.nodes.values())
            if (entry.visible) unique.set(addressKey(entry.node), entry.node);
        const nodes = [...unique.values()].slice(0, 64);
        try {
            const response = nodes.length ? await this.request({ type: "observationDescribe", nodes }) as { capabilities: ObservationCapability[] }
                : { capabilities: [] };
            if (epoch !== this.epoch || this.disposed) return;
            const capabilities: Capability[] = [];
            const stateNodes = new Set<number>();
            for (const capability of response.capabilities) {
                const node = nodes[capability.nodeIndex];
                if (!node) continue;
                // All output meters and one supported numeric State field per
                // node prove the first two registry-backed operations.
                if (capability.kind === "stateField" && stateNodes.has(capability.nodeIndex)) continue;
                if (capability.kind === "stateField") stateNodes.add(capability.nodeIndex);
                if (capability.operations.length === 0) continue;
                const channels = capability.kind === "output" ? capability.channels ?? [] : [undefined];
                // Keep every channel of an advertised port together at the
                // viewport budget boundary; never show half a stereo pair.
                if (channels.length > 64 - capabilities.length) continue;
                for (const channel of channels) {
                    const source: ObservationSource = { node, kind: capability.kind, member: capability.member,
                        ...(channel ? { channel: channel.index } : {}) };
                    const operation = capability.kind === "output" ? "level" : capability.operations[0];
                    if (!capability.operations.includes(operation)) continue;
                    const id = this.id(source, operation);
                    capabilities.push({ ...capability, source, id, operation, channelName: channel?.name });
                    if (capabilities.length === 64) break;
                }
                if (capabilities.length === 64) break;
            }
            this.install(capabilities);
            await this.reconcile(this.desired);
        } catch (error) { if (!this.disposed) this.error.value = String(error); }
    }
    private id(source: ObservationSource, operation: string): string {
        const key = `${observationSourceKey(source)}:${operation}`;
        let id = this.sourceIds.get(key);
        if (!id) { id = `probe:${++this.nextSourceId}`; this.sourceIds.set(key, id); }
        return id;
    }
    mode(node: NodeAddress, member: string): MeterMode {
        return this.modes.get(observationSourceKey({ node, kind: "output", member })) ?? "level";
    }
    setMode(node: NodeAddress, member: string, mode: MeterMode): void {
        const key = observationSourceKey({ node, kind: "output", member });
        this.modes.set(key, mode);
        // Both scales display the same level operation. A visual scale switch
        // keeps readings, probe identity and the active executable unchanged.
        this.preferences?.save(Object.fromEntries(this.modes));
        ++this.version.value;
    }
    hint(owner: symbol, value: ObservationHint | null): void {
        this.hints.delete(owner);
        if (value) this.hints.set(owner, value);
        this.publishHint([...this.hints.values()].at(-1) ?? null);
    }
    private install(capabilities: Capability[]): void {
        const activeIds = new Set(capabilities.map((entry) => entry.id));
        for (const id of this.values.values.keys()) if (!activeIds.has(id)) this.values.values.delete(id);
        // Retain a small recent identity cache across presentation remounts.
        if (this.sourceIds.size > 4096) for (const [key, id] of this.sourceIds)
            if (!activeIds.has(id)) this.sourceIds.delete(key);
        if (this.modes.size > 4096) {
            const active = new Set(capabilities.filter((entry) => entry.kind === "output")
                .map((entry) => observationSourceKey({ ...entry.source, channel: undefined })));
            for (const key of this.modes.keys()) if (!active.has(key)) this.modes.delete(key);
        }
        this.capabilities.value = capabilities;
        this.desired = capabilities.map((capability) => ({ id: capability.id, source: capability.source,
            operation: capability.operation }));
        ++this.version.value;
    }
    private reconcile(interests: ObservationInterest[]): Promise<void> {
        this.queue = this.queue.catch(() => {}).then(async () => {
            if (this.disposed || interests !== this.desired) return;
            const result = await this.request({ type: "observationReconcile", interests }) as { revision: string };
            if (this.disposed) return;
            this.values.acceptRevision(result.revision);
            this.error.value = "";
        }).catch((error) => { if (!this.disposed) this.error.value = String(error); });
        return this.queue;
    }
    disconnect(): void {
        ++this.epoch;
        this.values.clear();
        this.capabilities.value = [];
        ++this.version.value;
        this.desired = [];
        this.hints.clear();
        this.publishHint(null);
        for (const request of this.pending.values()) {
            clearTimeout(request.timer);
            request.reject(new Error("Observation session disconnected"));
        }
        this.pending.clear();
    }
    dispose(): void {
        if (this.timer) clearTimeout(this.timer);
        clearInterval(this.heartbeat);
        this.disconnect();
        this.disposed = true;
    }
}
const key: InjectionKey<PresentationObservations> = Symbol("iv.observations");
export function provideObservations(value: PresentationObservations): void { provide(key, value); }
export function useObservations(): PresentationObservations | null { return inject(key, null); }
