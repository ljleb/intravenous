import { randomUUID } from "node:crypto";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { Duplex } from "node:stream";
import { setTimeout as delay } from "node:timers/promises";
import { compareInteractionSends, isInteractionSend, ObservationTraceInput, type ClientInteractionTrace, type JsonRpcSend, type Timed } from "./clientInteractionTrace";
import { addressKey, graphScopeWithChildren, type GraphScope } from "./graphScope";
import { runPresentationScenario, type PresentationScenario } from "./headlessPresentation";
import { ManualControlInteractions } from "./manualControls";
import { answerNodeEventRequest } from "./nodeEventMessaging";
import { ObservationDeliveryState, type ObservationDelivery } from "./observationDelivery";
import { answerObservationRequest } from "./observationEndpoint";
import { recordedEventTimestampsOperation, recordedTriggerBinsOperation, type ObservationSource,
    type ObservationWindowResult } from "./observations";
import { PresentationObservations } from "./presentationObservations";
import { JsonRpcSocketClient } from "./rpcClient";
import { WorkspaceRpc, workspaceNodeEvents } from "./workspaceRpc";
import { spawnWorkspaceServer } from "./workspaceServerProcess";

const usage = `Usage:
  node headlessClient.js inspect --server PATH --project DIR
  node headlessClient.js replay --server PATH --project DIR --trace FILE [--steady]
  node headlessClient.js present --server PATH --project DIR --scenario FILE

Use a copy of the project for replay. Knob edits can be saved by the server.
--steady keeps the initial observation interests while replaying knob actions.
Output is JSON. Server logs go to stderr.`;

type Options = { command: "inspect" | "replay" | "present"; server: string; project: string; trace?: string;
    scenario?: string; steady?: boolean };
function parseOptions(args: string[]): Options {
    const [command, ...rest] = args;
    if (command !== "inspect" && command !== "replay" && command !== "present") throw new Error(usage);
    const options = new Map<string, string>();
    for (let i = 0; i < rest.length; ++i) {
        const flag = rest[i];
        if (options.has(flag)) throw new Error(`Duplicate option ${flag}`);
        if (flag === "--steady" && command === "replay") options.set(flag, "true");
        else if (flag === "--server" || flag === "--project" || (flag === "--trace" && command === "replay")
            || (flag === "--scenario" && command === "present")) {
            const value = rest[++i];
            if (!value || value.startsWith("--")) throw new Error(`Missing value for ${flag}`);
            options.set(flag, value);
        } else throw new Error(`Unknown option ${flag}`);
    }
    const server = options.get("--server"), project = options.get("--project"), trace = options.get("--trace");
    const scenario = options.get("--scenario");
    if (!server || !project || (command === "replay" && !trace) || (command === "present" && !scenario)) throw new Error(usage);
    return { command, server: resolve(server), project: resolve(project), trace, scenario, steady: options.has("--steady") };
}

async function readTrace(path: string): Promise<ClientInteractionTrace> {
    const trace = JSON.parse(await readFile(path, "utf8")) as ClientInteractionTrace;
    if (trace?.version !== 1 || !Number.isInteger(trace.durationMs) || trace.durationMs <= 0 || trace.durationMs > 25_000
        || typeof trace.owner !== "string" || !trace.view || !trace.baseline || !Array.isArray(trace.expanded)
        || !Array.isArray(trace.baseline.inputs) || !Array.isArray(trace.baseline.sourceIds)
        || !Array.isArray(trace.events) || !Array.isArray(trace.sent)) throw new Error("Invalid client interaction trace");
    if (trace.truncated) throw new Error("The trace reached its recording limit. Record a shorter interaction.");
    if (trace.events.some(({ atMs }, i) => !Number.isFinite(atMs) || atMs < 0
        || atMs > trace.durationMs + 1_000 || (i > 0 && atMs < trace.events[i - 1].atMs)))
        throw new Error("The trace contains invalid event times");
    return trace;
}

function within<T>(work: Promise<T>, milliseconds: number, message: string): Promise<T> {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(message)), milliseconds);
        void work.then(resolve, reject).finally(() => clearTimeout(timer));
    });
}

async function run(options: Options, trace?: ClientInteractionTrace) {
    const child = spawnWorkspaceServer(options.server, options.project, 2);
    const owner = randomUUID();
    let revision = 0;
    let connected = true;
    let startupFailure: Error | undefined;
    let started = 0;
    let recording = false;
    const sent: Timed<JsonRpcSend>[] = [];
    const graphEvents: Timed<{ method: string; params: Record<string, unknown> }>[] = [];
    let receiveObservation: ((update: ObservationDelivery) => void) | undefined;
    let refreshScope: (() => void) | undefined;
    let onReady: (() => void) | undefined;
    let failReady: ((error: Error) => void) | undefined;
    const ready = new Promise<void>((resolve, reject) => { onReady = resolve; failReady = reject; });
    const socket = child.stdio[3];
    if (!(socket instanceof Duplex)) throw new Error("The server process has no RPC socket");
    const client = new JsonRpcSocketClient(socket, (method, params) => {
        if (typeof params.revision === "number") revision = Math.max(revision, params.revision);
        if (method === "server.ready") {
            if (typeof params.sessionRevision === "number") revision = params.sessionRevision;
            onReady?.();
        } else if (method === "server.status" && params.code === "startupFailed") {
            startupFailure = new Error(String(params.message ?? "Server startup failed"));
            failReady?.(startupFailure);
        } else if (method === "observations.updated") receiveObservation?.(params as ObservationDelivery);
        else if (method === "graph.changed" || method === "session.revisionChanged" || method === "ivPackages.updated") {
            if (recording && graphEvents.length < 8192) graphEvents.push({ atMs: performance.now() - started, value: { method, params } });
            refreshScope?.();
        }
    }, (error) => { connected = false; failReady?.(startupFailure ?? error); }, (current) => { revision = current; });
    child.on("error", (error) => { startupFailure = error; failReady?.(error); });
    const rpc = new WorkspaceRpc(client, () => revision);
    const stopObserving = client.observeSends((value) => {
        if (recording && sent.length < 8192 && isInteractionSend(value, owner))
            sent.push({ atMs: performance.now() - started, value });
    });
    let observations: PresentationObservations | undefined;
    const manual = new ManualControlInteractions(() => connected ? rpc : null, () => {});
    const nodeEvents = workspaceNodeEvents(() => connected ? rpc : null, () => revision, (error) => console.error(String(error)));
    try {
        await client.connect();
        await within(ready, 20_000, "The server did not report ready");
        await rpc.hello(`headless-${process.pid}-${owner}`, "");
        if (options.scenario) {
            const scenario = JSON.parse(await readFile(options.scenario, "utf8")) as PresentationScenario;
            return await runPresentationScenario(rpc, nodeEvents, owner, scenario);
        }
        if (!trace) return { scope: await rpc.getGraphScope(), playback: await rpc.getPlayback(true) };

        let expanded = trace.expanded;
        let view = trace.view;
        const delivery = new ObservationDeliveryState();
        const recordedEvents = new Map<string, Map<string, number>>();
        const triggerWindows = new Map<string, { source?: ObservationSource; window: ObservationWindowResult["window"];
            bins: NonNullable<ObservationWindowResult["triggerBins"]> }>();
        let triggerBinCount = 0;
        const eventSources = new Map<string, ObservationSource>();
        let incompleteEventWindow = false;
        observations = new PresentationObservations((request) => {
            if (request.type === "observationReconcile") for (const interest of request.interests)
                if (interest.operation === recordedEventTimestampsOperation || interest.operation === recordedTriggerBinsOperation)
                    eventSources.set(interest.id, interest.source);
            void answerObservationRequest(rpc, view, owner, request).then((reply) => observations?.receive(reply));
        });
        receiveObservation = (update) => {
            delivery.receive(update);
            if (view.visible) observations?.receiveUpdate({ ...update, scopes: owner in update.scopes
                ? { viewport: update.scopes[owner] } : {} });
            if (recording) for (const [id, window] of delivery.changes(owner, update).windows) {
                if (!eventSources.has(id) || window?.status !== "ready") continue;
                incompleteEventWindow ||= !!window.truncated;
                if (window.triggerBins) {
                    const key = JSON.stringify([id, window.window.begin, window.window.length, window.window.points]);
                    const bins = window.triggerBins;
                    const count = triggerBinCount - (triggerWindows.get(key)?.bins.length ?? 0) + bins.length;
                    if (count > 65_536 || (!triggerWindows.has(key) && triggerWindows.size >= 1024)) {
                        incompleteEventWindow = true;
                        continue;
                    }
                    triggerBinCount = count;
                    triggerWindows.set(key, { source: eventSources.get(id), window: window.window, bins });
                    continue;
                }
                let indices = recordedEvents.get(id);
                if (!indices) { indices = new Map(); recordedEvents.set(id, indices); }
                const counts = new Map<string, number>();
                for (const index of window.eventIndices ?? []) {
                    if (!indices.has(index) && indices.size >= 65_536) { incompleteEventWindow = true; break; }
                    const count = (counts.get(index) ?? 0) + 1;
                    counts.set(index, count);
                    indices.set(index, Math.max(indices.get(index) ?? 0, count));
                }
            }
        };

        const requestedNodes = new Set(trace.baseline.inputs.flatMap((input) => input.type === "lane" ? [addressKey(input.node)] : []));
        const scopeDeadline = performance.now() + 60_000;
        let scope: GraphScope;
        for (;;) {
            scope = graphScopeWithChildren(await rpc.getGraphScope(expanded));
            const present = new Set(scope.nodes.map(({ address }) => addressKey(address)));
            if (scope.generation !== null && scope.execution?.phase !== "configurationFailed"
                && scope.execution?.activeGeneration === scope.generation
                && [...requestedNodes].every((key) => present.has(key))) break;
            if (performance.now() >= scopeDeadline) throw new Error(scope.execution?.message
                || "The recorded graph did not become active. Use a copy of the project from the recording.");
            await delay(100);
        }
        manual.reconcile(scope);
        let dirty = false, reading = false;
        let refreshError: unknown;
        const updateScope = async () => {
            dirty = true;
            if (reading) return;
            reading = true;
            try {
                while (dirty && connected) {
                    dirty = false;
                    const latest = await rpc.getGraphScope(expanded);
                    if (!dirty) manual.reconcile(graphScopeWithChildren(latest));
                }
            } catch (error) { refreshError = error; }
            finally { reading = false; }
        };
        refreshScope = () => { void updateScope(); };

        if (trace.playback) await rpc.seekPlayback(trace.playback.position);
        const input = new ObservationTraceInput(observations);
        observations.restoreIdentities(trace.baseline);
        for (const event of trace.baseline.inputs) input.apply(event);
        await observations.settled(20_000);
        if (trace.playback) await rpc.setPlaybackRunning(trace.playback.running);

        const profileId = randomUUID();
        const profileStarted = await rpc.getPlayback(true, { id: profileId, durationMs: trace.durationMs + 500 });
        const failures: Array<{ atMs: number; error: string }> = [];
        const pendingEdits = new Set<Promise<void>>();
        const until = (atMs: number) => {
            const remaining = started + atMs - performance.now();
            return remaining > 0 ? delay(remaining) : undefined;
        };
        started = performance.now();
        recording = true;
        for (const { atMs, value } of trace.events) {
            const wait = until(atMs);
            if (wait) await wait;
            switch (value.type) {
            case "observation":
                if (!options.steady || value.input.type === "refresh" || value.input.type === "disconnect")
                    input.apply(value.input);
                break;
            case "manual":
                try { manual.action(owner, value.action); }
                catch (error) { failures.push({ atMs, error: String(error) }); }
                break;
            case "nodeEvent": {
                const edit = answerNodeEventRequest(nodeEvents, owner, value.request).then((reply) => {
                    if (reply.error) failures.push({ atMs, error: reply.error });
                }).finally(() => { pendingEdits.delete(edit); });
                pendingEdits.add(edit);
                break;
            }
            case "view":
                if (options.steady) break;
                view = { visible: value.visible, active: value.active };
                if (!view.visible) void rpc.reconcileObservations(owner, []).catch((error) => failures.push({ atMs, error: String(error) }));
                break;
            case "expanded":
                expanded = value.nodes;
                refreshScope();
                break;
            }
        }
        await until(trace.durationMs);
        recording = false;
        await within(manual.settled(), 20_000, "Knob interactions did not settle");
        await within(Promise.all(pendingEdits), 20_000, "Presentation edits did not settle");
        await observations.settled(20_000);
        await until(trace.durationMs + 510);
        const playback = await rpc.getPlayback(true, { id: profileId });
        return { steady: !!options.steady, comparison: options.steady ? undefined : compareInteractionSends(trace.sent, trace.owner, sent, owner),
            failures, controlStates: manual.states(owner), refreshError: refreshError && String(refreshError),
            graphEvents, sent, playbackStart: profileStarted.diagnostics, playback,
            recordedEvents: [...recordedEvents].map(([id, values]) => ({ source: eventSources.get(id),
                indices: [...values].sort(([a], [b]) => BigInt(a) < BigInt(b) ? -1 : BigInt(a) > BigInt(b) ? 1 : 0)
                    .flatMap(([index, count]) => Array<string>(count).fill(index)) })),
            recordedTriggerWindows: [...triggerWindows.values()], incompleteEventWindow };
    } finally {
        recording = false;
        stopObserving();
        observations?.dispose();
        manual.disconnect();
        nodeEvents.disconnected();
        client.dispose();
        child.kill();
    }
}

async function main() {
    const args = process.argv.slice(2);
    if (!args.length || args.includes("--help") || args.includes("-h")) { console.log(usage); return; }
    const options = parseOptions(args);
    const trace = options.trace ? await readTrace(options.trace) : undefined;
    console.log(JSON.stringify(await run(options, trace),
        (_key, value) => value instanceof Float64Array ? Array.from(value) : value, 2));
}

void main().catch((error) => {
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 1;
});
