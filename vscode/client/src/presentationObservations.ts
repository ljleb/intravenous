import type { NodeAddress } from "./graphScope";
import { addressFromKey, addressKey } from "./graphScope";
import type { ObservationProfile } from "./observationProfile";
import type { ObservationDelivery } from "./observationDelivery";
import { observationWindowId,
    observationWindowIds, observationWindowRequests, visibleObservationRequests, type ObservationTier } from "./observationWindows";
import type { ObservationViewportDisplay } from "./observationViewportDisplay";
import { ObservationValues, observationSourceKey, recordedTriggerBinsOperation, type ObservationCapability, type ObservationHostRequest,
    type ObservationInterest, type ObservationResponse, type ObservationSource, type ObservationTimeSpan,
    type MeterMode, type ObservationWindow, sameObservationWindow } from "./observations";

type NodeInterest = { node: NodeAddress; tier?: ObservationTier; window?: ObservationWindow; firstInput?: string };
export type PortObservation = Omit<ObservationInterest, "id"> & { prefetchRank?: number };
type PresentationInterest = PortObservation & { id: string };
type ViewportObservation = { source: ObservationSource; operation: string; window: ObservationWindow; consumer?: NodeAddress | string };
export type ObservationViewport = { id: string; visible: ObservationTimeSpan; requested: ObservationWindow; completeOnly: boolean };
export type PresentedObservation = ObservationCapability & { source: ObservationSource; id: string; operation: string; channelName?: string };
type Capability = PresentedObservation;

export type ObservationInput =
    | ({ type: "lane"; owner: number } & NodeInterest)
    | { type: "removeLane"; owner: number }
    | { type: "predict"; nodes: readonly NodeAddress[] }
    | { type: "port"; id: string; observation?: PortObservation; scheduling?: "viewport"; consumer?: NodeAddress | string }
    | { type: "visible"; visible: boolean }
    | { type: "refresh" }
    | { type: "disconnect" };

export type ObservationBaseline = {
    inputs: ObservationInput[];
    sourceIds: [string, string][];
    nextSourceId: number;
};
const maximumLiveProbes = 256;
const maximumWindows = 256;
const capabilityCacheLimit = 1024;
const discoveryBatchMs = 75;
const meterModeKey = ({ node, kind, member }: ObservationSource) => observationSourceKey({ node, kind, member });
const laneEligibilityKey = (node: NodeAddress) => `lane:${addressKey(node)}`;
export type ObservationStorage = {
    cell<T>(value: T): { value: T };
    map<T>(value: Map<string, T>): Map<string, T>;
};

export type ObservationScheduler = {
    after(ms: number, run: () => void): () => void;
    every(ms: number, run: () => void): () => void;
};

const defaultScheduler: ObservationScheduler = {
    after(ms, run) {
        const timer = setTimeout(run, ms);
        return () => clearTimeout(timer);
    },
    every(ms, run) {
        const timer = setInterval(run, ms);
        return () => clearInterval(timer);
    },
};

const plainStorage: ObservationStorage = {
    cell: (value) => ({ value }),
    map: (value) => value,
};

export class PresentationObservations {
    readonly capabilities: { value: Capability[] };
    private readonly capabilitiesByNode: Map<string, Capability[]>;
    readonly values: ObservationValues;
    readonly error: { value: string };
    private nodes = new Map<symbol, NodeInterest>();
    private explicit = new Map<string, PortObservation>();
    private explicitConsumers = new Map<string, NodeAddress | string>();
    private nextExplicitId = 0;
    private visible = true;
    private pending = new Map<string, { resolve(value: unknown): void; reject(error: Error): void; cancel: () => void }>();
    private timer?: () => void;
    private windowTimer?: () => void;
    private viewportWindowScheduled = false;
    private discovering = false;
    private discoveryNeeded = false;
    private readonly described = new Map<string, ObservationCapability[]>();
    private readonly stopHeartbeat: () => void;
    private sequence = 0;
    private sourceIds = new Map<string, string>();
    private nextSourceId = 0;
    private epoch = 0;
    private disposed = false;
    private readonly modes: Map<string, MeterMode>;
    private desired: ObservationInterest[] = [];
    private submitted: ObservationInterest[] = [];
    private accepted: ObservationInterest[] | undefined = [];
    private predictedNodes = new Set<string>();
    private acceptedPrediction = "";
    private visibilityVersion = 0;
    private profile?: { recorder: ObservationProfile; name: (address: NodeAddress) => string };
    private queue = Promise.resolve();
    private recording?: { sink: (input: ObservationInput) => void; owners: Map<symbol, number>; nextOwner: number };
    constructor(private readonly send: (request: ObservationHostRequest) => void,
        private readonly preferences?: { initial?: Record<string, MeterMode>; save(modes: Record<string, MeterMode>): void },
        storage: ObservationStorage = plainStorage, private readonly scheduler: ObservationScheduler = defaultScheduler) {
        this.capabilities = storage.cell<Capability[]>([]);
        this.capabilitiesByNode = storage.map(new Map());
        this.values = new ObservationValues("viewport", storage.map);
        this.error = storage.cell("");
        this.modes = storage.map(new Map());
        for (const [key, mode] of Object.entries(preferences?.initial ?? {}).slice(0, 4096))
            if (mode === "level" || mode === "value") this.modes.set(key, mode);
        this.stopHeartbeat = scheduler.every(10000, () => { if (this.desired.length) void this.reconcile(this.desired, true); });
    }
    startRecording(sink: (input: ObservationInput) => void): ObservationBaseline {
        const owners = new Map<symbol, number>();
        const inputs: ObservationInput[] = [{ type: "visible", visible: this.visible }];
        for (const [owner, interest] of this.nodes) {
            const id = owners.size;
            owners.set(owner, id);
            inputs.push({ type: "lane", owner: id, ...interest });
        }
        for (const [id, observation] of this.explicit) {
            inputs.push({ type: "port", id, observation, consumer: this.explicitConsumers.get(id) });
        }
        inputs.push({ type: "predict", nodes: [...this.predictedNodes].map(addressFromKey) });
        this.recording = { sink, owners, nextOwner: owners.size };
        return { inputs, sourceIds: [...this.sourceIds], nextSourceId: this.nextSourceId };
    }
    stopRecording(): void { this.recording = undefined; }

    restoreIdentities({ sourceIds, nextSourceId }: ObservationBaseline): void {
        this.sourceIds = new Map(sourceIds);
        this.nextSourceId = nextSourceId;
    }

    async settled(timeoutMs: number): Promise<void> {
        const deadline = performance.now() + timeoutMs;
        while (this.timer || this.windowTimer || this.discovering || this.pending.size) {
            if (performance.now() >= deadline) throw new Error("Observation requests did not settle");
            await new Promise<void>((resolve) => setTimeout(resolve, 5));
        }
        await this.queue;
        if (this.error.value) throw new Error(this.error.value);
    }

    private request(value: Omit<Extract<ObservationHostRequest, { type: "observationDescribe" }>, "requestId">
        | Omit<Extract<ObservationHostRequest, { type: "observationReconcile" }>, "requestId">): Promise<unknown> {
        const requestId = `observation:${++this.sequence}`;
        return new Promise((resolve, reject) => {
            const cancel = this.scheduler.after(30000, () => {
                this.pending.delete(requestId);
                reject(new Error("Observation request timed out"));
            });
            this.pending.set(requestId, { resolve, reject, cancel });
            try {
                // Node addresses come from Vue's reactive graph state. Only
                // plain JSON data can cross the webview's postMessage boundary.
                const message = JSON.parse(JSON.stringify({ ...value, requestId })) as ObservationHostRequest;
                this.send(message);
            } catch (error) {
                cancel();
                this.pending.delete(requestId);
                reject(error);
            }
        });
    }
    receiveUpdate(update: ObservationDelivery): boolean {
        return this.values.receive(update, this.profile ? this.profileUsable : undefined);
    }
    receive(message: ObservationResponse): void {
        const pending = this.pending.get(message.requestId);
        if (!pending) return;
        this.pending.delete(message.requestId);
        pending.cancel();
        if (message.error) pending.reject(new Error(message.error));
        else pending.resolve(message.result);
    }
    node(owner: symbol, node: NodeAddress, tier: ObservationTier | undefined, window?: ObservationWindow, firstInput?: string): void {
        const previous = this.nodes.get(owner);
        const sameLane = previous && previous.tier === tier && addressKey(previous.node) === addressKey(node)
            && previous.firstInput === firstInput;
        if (sameLane && sameObservationWindow(previous.window, window)) return;
        if (this.recording) {
            const { owners, sink } = this.recording;
            if (!owners.has(owner)) owners.set(owner, this.recording.nextOwner++);
            sink({ type: "lane", owner: owners.get(owner)!, node, tier, window, firstInput });
        }
        this.nodes.set(owner, { node, tier, window, firstInput });
        if (this.profile && (previous?.tier !== tier || !previous || addressKey(previous.node) !== addressKey(node))) {
            if (previous?.tier === "visible" && addressKey(previous.node) !== addressKey(node)) this.profileLaneEligibility(previous.node);
            this.profileLaneEligibility(node);
        }
        if (sameLane) {
            if (tier && this.visible && this.forNode(node).some((capability) => capability.delivery === "window"))
                this.scheduleWindows(tier === "visible" ? "viewport" : undefined);
        } else if (tier || previous?.tier) {
            this.schedule();
            if (tier === "visible" && this.forNode(node).length) this.scheduleWindows("viewport");
        }
    }
    remove(owner: symbol): void {
        const previous = this.nodes.get(owner);
        const recorded = this.recording?.owners.get(owner);
        if (recorded !== undefined) {
            this.recording!.owners.delete(owner);
            this.recording!.sink({ type: "removeLane", owner: recorded });
        }
        if (this.nodes.delete(owner) && previous?.tier) {
            if (this.profile) this.profileLaneEligibility(previous.node);
            this.schedule();
        }
    }
    predictViewport(nodes: readonly NodeAddress[]): void {
        const keys = new Set(nodes.map(addressKey));
        if (keys.size === this.predictedNodes.size && [...keys].every((key) => this.predictedNodes.has(key))) return;
        this.recording?.sink({ type: "predict", nodes });
        this.predictedNodes = keys;
        this.schedule();
    }
    portObservationId(): string { return `port:${++this.nextExplicitId}`; }
    observePort(id: string, next: PortObservation | undefined, scheduling?: "viewport", consumer?: NodeAddress | string): void {
        if (next && consumer !== undefined) this.explicitConsumers.set(id, consumer);
        else this.explicitConsumers.delete(id);
        const previous = this.explicit.get(id);
        const sameSource = previous && next && observationSourceKey(previous.source) === observationSourceKey(next.source)
            && previous.operation === next.operation && previous.window?.maxDepth === next.window?.maxDepth
            && !!previous.window?.includeIndexedRuns === !!next.window?.includeIndexedRuns;
        if (sameSource && sameObservationWindow(previous.window, next.window) && previous.prefetch === next.prefetch
            && previous.prefetchRank === next.prefetchRank) return;
        this.recording?.sink({ type: "port", id, observation: next, scheduling, consumer });
        if (next) this.explicit.set(id, next);
        else if (!this.explicit.delete(id)) return;
        const eligible = !!next && !next.prefetch;
        if (eligible !== (!!previous && !previous.prefetch)) this.profile?.recorder.recordEligibility(id, this.visible && eligible);
        if (sameSource && previous.window && next.window) { this.scheduleWindows(scheduling); return; }
        this.values.values.delete(id);
        if (next) this.values.prepareWindow(id, next);
        this.values.retireWindow(id);
        this.schedule();
        if (scheduling === "viewport") this.scheduleWindows(scheduling);
    }
    observeViewport(id: string, observation?: ViewportObservation, tier?: ObservationTier): void {
        const next = new Set<string>();
        if (observation && tier) {
            const { source, operation, window, consumer } = observation;
            const requests = operation === recordedTriggerBinsOperation
                ? tier === "visible" ? visibleObservationRequests(window) : []
                : observationWindowRequests(window, tier);
            for (const { slot, ...request } of requests) {
                const key = observationWindowId(id, slot);
                this.observePort(key, { source, operation, ...request }, tier === "visible" ? "viewport" : undefined, consumer);
                next.add(key);
            }
        }
        for (const key of observationWindowIds(id)) if (!next.has(key)) this.observePort(key, undefined);
    }
    setVisible(visible: boolean): void {
        if (this.visible === visible) return;
        this.recording?.sink({ type: "visible", visible });
        this.visible = visible;
        if (this.profile) {
            for (const { node, tier } of this.nodes.values()) if (tier === "visible")
                this.profile.recorder.recordEligibility(laneEligibilityKey(node), visible);
            for (const [id, entry] of this.explicit) if (!entry.prefetch)
                this.profile.recorder.recordEligibility(id, visible);
        }
        ++this.visibilityVersion;
        // The extension releases a hidden panel's server scope immediately.
        this.accepted = undefined;
        this.schedule();
    }
    windowForDisplay(id: string, visible?: ObservationTimeSpan) {
        return this.values.windowForDisplay(id, visible);
    }
    viewportWindows(display: ObservationViewportDisplay, viewports: readonly ObservationViewport[]) {
        return display.read(viewports.map(({ id, visible, requested, completeOnly }) => {
            const fine = visibleObservationRequests(requested);
            return { id, visible, completeOnly, fine: fine.map(({ window }) => window),
                current: fine.map(({ slot, window }) => this.values.currentWindow(observationWindowId(id, slot), window)),
                ready: this.values.readyWindowsForDisplay(observationWindowIds(id), visible, completeOnly) };
        }), this.values.windowEpoch);
    }
    refresh(): void {
        this.recording?.sink({ type: "refresh" });
        ++this.epoch;
        this.described.clear();
        this.schedule();
    }
    startProfile(recorder: ObservationProfile, name: (address: NodeAddress) => string): void {
        this.profile = { recorder, name };
        recorder.recordRequests(this.submitted, this.profileLabel, true);
    }
    stopProfile(): void { this.profile = undefined; }
    private readonly profileUsable = (id: string) => this.profile?.recorder.recordUsable(id);
    private profileLaneEligibility(node: NodeAddress): void {
        const key = addressKey(node);
        // A concrete node may be shown under several symbolic lanes. Their
        // shared probe is eligible as long as any of those rows is visible.
        const visible = this.visible && Array.from(this.nodes.values()).some((entry) => entry.tier === "visible" && addressKey(entry.node) === key);
        this.profile?.recorder.recordEligibility(laneEligibilityKey(node), visible);
    }
    private readonly profileLabel = (interest: ObservationInterest) => {
        const name = this.profile!.name;
        const { source } = interest;
        const owner = this.explicitConsumers.get(interest.id) ?? source.node;
        const consumer = typeof owner === "string" ? owner : name(owner);
        const port = `${source.portKind === "event" ? "event " : ""}${source.kind} ${source.member}`;
        const channel = source.channel === undefined ? "" : ` channel ${source.channel}`;
        const eligibilityKey = this.explicit.has(interest.id) ? interest.id : laneEligibilityKey(source.node);
        return { consumer, source: `${name(source.node)} • ${port}${channel} • ${interest.operation}`, eligibilityKey };
    };
    private schedule(): void {
        this.discoveryNeeded = true;
        if (this.disposed || this.timer || this.discovering) return;
        // Batch the first change. Further scroll events must not move the
        // deadline or invalidate a description of the same graph.
        this.timer = this.scheduler.after(discoveryBatchMs, () => { this.timer = undefined; void this.discover(); });
    }
    private scheduleWindows(scheduling?: "viewport"): void {
        const viewport = scheduling === "viewport";
        if (this.windowTimer && (!viewport || this.viewportWindowScheduled)) return;
        this.windowTimer?.();
        this.viewportWindowScheduled = viewport;
        // Explicit viewport windows can use the current capability list while
        // discovery runs. Coalesce all windows in a frame, including other
        // interests already waiting for the normal update deadline.
        this.windowTimer = this.scheduler.after(viewport ? 16 : 100, () => {
            this.windowTimer = undefined;
            void this.updateInterests(this.explicitInterests());
        });
    }
    private explicitInterests(): PresentationInterest[] {
        // Explicit readers can receive an unavailable or pending result even
        // when discovery does not advertise a capability.
        return this.visible ? Array.from(this.explicit, ([id, interest]) => ({ ...interest, id })) : [];
    }
    private observedNodes() {
        const unique = new Map<string, NodeAddress>();
        const visible = new Set<string>();
        const firstInputs = new Map<string, string>();
        if (this.visible) {
            for (const entry of this.nodes.values()) {
                const key = addressKey(entry.node);
                if (!entry.tier && !this.predictedNodes.has(key)) continue;
                unique.set(key, entry.node);
                if (entry.tier === "visible") visible.add(key);
                if (entry.firstInput !== undefined) firstInputs.set(key, entry.firstInput);
            }
        }
        const nodes = [...unique].sort(([a], [b]) => Number(visible.has(b)) - Number(visible.has(a)))
            .slice(0, maximumLiveProbes + maximumWindows).map(([, node]) => node);
        return { nodes, visible, firstInputs };
    }
    private async discover(): Promise<void> {
        const epoch = this.epoch;
        this.discovering = true;
        this.discoveryNeeded = false;
        try {
            const missing = this.observedNodes().nodes.filter((node) => !this.described.has(addressKey(node)));
            if (missing.length) {
                const response = await this.request({ type: "observationDescribe", nodes: missing }) as { capabilities: ObservationCapability[] };
                if (epoch !== this.epoch || this.disposed) return;
                const entries = missing.map((): ObservationCapability[] => []);
                for (const capability of response.capabilities) entries[capability.nodeIndex]?.push(capability);
                missing.forEach((node, index) => this.described.set(addressKey(node), entries[index]));
            }
            const { nodes, visible, firstInputs } = this.observedNodes();
            const current = new Set(nodes.map(addressKey));
            if (this.described.size > capabilityCacheLimit) for (const key of this.described.keys()) {
                if (!current.has(key)) this.described.delete(key);
                if (this.described.size <= capabilityCacheLimit) break;
            }
            const described = nodes.flatMap((node, nodeIndex) =>
                (this.described.get(addressKey(node)) ?? []).map((capability) => ({ ...capability, nodeIndex })));
            const explicit = this.explicitInterests();
            const capabilities: Capability[] = [];
            const portCounts = new Map<number, number>();
            const portsByRank: ObservationCapability[][] = [];
            for (const capability of described) {
                const node = nodes[capability.nodeIndex];
                if (!node || capability.kind === "stateField") continue;
                const key = addressKey(node);
                if (capability.kind === "input" && firstInputs.get(key) !== capability.member) continue;
                const operation = capability.delivery === "window" ? "waveform" : "level";
                if (!capability.operations.includes(operation) || !capability.channels?.length) continue;
                const rank = portCounts.get(capability.nodeIndex) ?? 0;
                (portsByRank[rank] ??= []).push(capability);
                portCounts.set(capability.nodeIndex, rank + 1);
            }
            // Allocate one visible port per lane before spending the remaining
            // budget on extra ports.
            let remainingLive = maximumLiveProbes;
            let remainingWindows = maximumWindows;
            for (const interest of explicit) {
                if (interest.prefetch) continue;
                if (interest.window) --remainingWindows;
                else --remainingLive;
            }
            const admitted = new Set<ObservationCapability>();
            for (const capability of portsByRank.flat()) {
                const count = capability.channels?.length ?? 0;
                const window = capability.delivery === "window";
                // Keep discovered nearby live capabilities ready for promotion.
                // Only the visible lanes spend the compiled probe budget.
                if (!visible.has(addressKey(nodes[capability.nodeIndex]))) { admitted.add(capability); continue; }
                // Keep every channel of an advertised port together at the
                // viewport budget boundary; never show half a stereo pair.
                if (count > (window ? remainingWindows : remainingLive)) continue;
                admitted.add(capability);
                if (window) remainingWindows -= count;
                else remainingLive -= count;
            }
            // The order of the displayed ports is still the interface order.
            for (const capability of described) {
                if (!admitted.has(capability)) continue;
                const node = nodes[capability.nodeIndex];
                for (const channel of capability.channels ?? []) {
                    const source: ObservationSource = { node, kind: capability.kind, member: capability.member, channel: channel.index };
                    const operation = capability.delivery === "window" ? "waveform" : "level";
                    const id = this.id(source, operation, capability);
                    capabilities.push({ ...capability, source, id, operation, channelName: channel.name });
                }
            }
            this.install(capabilities, explicit);
            void this.updateInterests(explicit);
        } catch (error) { if (epoch === this.epoch && !this.disposed) this.error.value = String(error); }
        finally {
            this.discovering = false;
            if (this.discoveryNeeded) this.schedule();
        }
    }
    private id(source: ObservationSource, operation: string, capability: ObservationCapability): string {
        // Equivalent sources keep their display history across graph revisions.
        // A changed channel or result schema gets a new identity and must not
        // inherit an incompatible source's readings or animation.
        const key = JSON.stringify([observationSourceKey(source), operation, capability.delivery,
            capability.channels?.find((channel) => channel.index === source.channel),
            capability.resultSchemas?.find((schema) => schema.operation === operation)]);
        let id = this.sourceIds.get(key);
        if (!id) { id = `probe:${++this.nextSourceId}`; this.sourceIds.set(key, id); }
        return id;
    }
    mode(source: ObservationSource): MeterMode {
        return this.modes.get(meterModeKey(source)) ?? "level";
    }
    forNode(node: NodeAddress): readonly Capability[] {
        return this.capabilitiesByNode.get(addressKey(node)) ?? [];
    }
    setMode(source: ObservationSource, mode: MeterMode): void {
        this.modes.set(meterModeKey(source), mode);
        // Both scales display the same level operation. A visual scale switch
        // keeps readings, probe identity and the active executable unchanged.
        this.preferences?.save(Object.fromEntries(this.modes));
    }
    private install(capabilities: Capability[], explicit: ObservationInterest[]): void {
        const activeIds = new Set([...capabilities, ...explicit].map((entry) => entry.id));
        for (const id of this.values.values.keys()) if (!activeIds.has(id)) this.values.values.delete(id);
        // Retain a small recent identity cache across presentation remounts.
        if (this.sourceIds.size > 4096) for (const [key, id] of this.sourceIds)
            if (!activeIds.has(id)) this.sourceIds.delete(key);
        if (this.modes.size > 4096) {
            const active = new Set(capabilities.map((entry) => meterModeKey(entry.source)));
            for (const key of this.modes.keys()) if (!active.has(key)) this.modes.delete(key);
        }
        const groups = new Map<string, Capability[]>();
        for (const capability of capabilities) {
            const key = addressKey(capability.source.node);
            const entries = groups.get(key);
            if (entries) entries.push(capability);
            else groups.set(key, [capability]);
        }
        for (const key of this.capabilitiesByNode.keys()) if (!groups.has(key)) this.capabilitiesByNode.delete(key);
        for (const [key, entries] of groups) {
            const previous = this.capabilitiesByNode.get(key);
            // nodeIndex is local to the current discovery batch.
            if (!previous || entries.length !== previous.length || entries.some(({ nodeIndex, ...entry }, index) => {
                const { nodeIndex: oldIndex, ...old } = previous[index];
                return JSON.stringify(entry) !== JSON.stringify(old);
            })) this.capabilitiesByNode.set(key, entries);
        }
        this.capabilities.value = capabilities;
    }
    private updateInterests(explicit: PresentationInterest[]): Promise<void> {
        const activeNodes = new Map<string, NodeInterest>();
        if (this.visible) for (const entry of this.nodes.values()) {
            if (!entry.tier) continue;
            const key = addressKey(entry.node);
            const previous = activeNodes.get(key);
            if (!previous || (previous.tier !== "visible" && entry.tier === "visible")
                || (previous.tier === entry.tier && entry.window && !previous.window)) activeNodes.set(key, entry);
        }
        const automatic = this.capabilities.value.flatMap<PresentationInterest>((capability) => {
            const key = addressKey(capability.source.node);
            const entry = activeNodes.get(key);
            if (!entry?.tier) return [];
            const { id, source, operation } = capability;
            if (capability.delivery === "window") {
                if (!entry.window) return [];
                return observationWindowRequests(entry.window, entry.tier).map((request) => ({
                    id: observationWindowId(id, request.slot), source, operation,
                    window: request.window, prefetch: request.prefetch, prefetchRank: request.prefetchRank,
                }));
            }
            return entry.tier === "visible" ? [{ id, source, operation }] : [];
        });
        let live = 0, windows = 0;
        const interests = [...explicit, ...automatic];
        const previews = interests.filter((interest) => interest.prefetch)
            .sort((a, b) => (a.prefetchRank ?? 0) - (b.prefetchRank ?? 0));
        this.desired = [...interests.filter((interest) => !interest.prefetch), ...previews].flatMap(({ prefetchRank, ...interest }) => {
            const admitted = interest.window ? ++windows <= maximumWindows : ++live <= maximumLiveProbes;
            if (!admitted) this.profile?.recorder.recordBudgetSkip(interest, this.profileLabel);
            return admitted ? [interest] : [];
        });
        return this.reconcile(this.desired);
    }
    private reconcile(interests: ObservationInterest[], renewLease = false): Promise<void> {
        this.queue = this.queue.catch(() => {}).then(async () => {
            if (this.disposed || interests !== this.desired) return;
            const accepted = this.accepted;
            const predictedInterests = this.predictedInterests(interests);
            const prediction = predictedInterests.map(({ id }) => id).join(",");
            if (!renewLease && !this.error.value && prediction === this.acceptedPrediction && interests.length === accepted?.length
                && interests.every((interest, index) => {
                    const old = accepted[index];
                    return interest.id === old.id && interest.operation === old.operation
                        && !!interest.prefetch === !!old.prefetch && sameObservationWindow(interest.window, old.window)
                        && observationSourceKey(interest.source) === observationSourceKey(old.source);
                })) return;
            this.values.expectWindows(interests);
            this.submitted = interests;
            this.profile?.recorder.recordRequests(interests, this.profileLabel);
            const visibilityVersion = this.visibilityVersion;
            const result = await this.request({ type: "observationReconcile", interests,
                ...(predictedInterests.length ? { predictedInterests } : {}),
            }) as { revision: string };
            if (this.disposed) return;
            if (visibilityVersion === this.visibilityVersion) {
                this.accepted = interests;
                this.acceptedPrediction = prediction;
            }
            this.values.acceptRevision(result.revision);
            this.error.value = "";
        }).catch((error) => { if (!this.disposed) this.error.value = String(error); });
        return this.queue;
    }
    private predictedInterests(active: ObservationInterest[]): ObservationInterest[] {
        if (!this.visible || !this.predictedNodes.size) return [];
        for (const { node } of this.nodes.values()) {
            const key = addressKey(node);
            if (this.predictedNodes.has(key) && !this.described.has(key)) return [];
        }
        const explicit = active.filter(({ window, id }) => !window && this.explicit.has(id));
        const automatic = this.capabilities.value.filter(({ delivery, source }) =>
            delivery !== "window" && this.predictedNodes.has(addressKey(source.node)));
        if (explicit.length + automatic.length > maximumLiveProbes) return [];
        return [...explicit, ...automatic.map(({ id, source, operation }) => ({ id, source, operation }))]
            .sort((a, b) => a.id.localeCompare(b.id));
    }
    disconnect(): void {
        this.recording?.sink({ type: "disconnect" });
        ++this.epoch;
        ++this.visibilityVersion;
        this.described.clear();
        this.discoveryNeeded = false;
        this.timer?.();
        this.timer = undefined;
        this.windowTimer?.();
        this.windowTimer = undefined;
        this.values.clear();
        this.capabilitiesByNode.clear();
        this.capabilities.value = [];
        this.desired = [];
        this.submitted = [];
        this.accepted = [];
        this.acceptedPrediction = "";
        this.profile?.recorder.recordRequests([], this.profileLabel, true);
        for (const request of this.pending.values()) {
            request.cancel();
            request.reject(new Error("Observation session disconnected"));
        }
        this.pending.clear();
    }
    dispose(): void {
        this.stopHeartbeat();
        this.disconnect();
        this.disposed = true;
    }
}
