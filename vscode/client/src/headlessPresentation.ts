import { randomUUID } from "node:crypto";
import { setTimeout as delay } from "node:timers/promises";
import { createContext, Script } from "node:vm";
import type { Timed } from "./clientInteractionTrace";
import { addressKey, graphScopeWithChildren, type NodeAddress } from "./graphScope";
import { NodeEventMessaging, answerNodeEventRequest } from "./nodeEventMessaging";
import type { NodeEventInteractions } from "./nodeEventInteractions";
import { PresentationLaneEdits } from "./presentationLaneEdits";
import type { PresentationFrame, PresentationInput, PresentationSnapshot } from "./presentationPlugin";
import { PresentationPluginSession, PresentationWake } from "./presentationPluginSession";
import { builtinPresentationPrograms, projectProgramsByDefinition } from "./presentationPrograms";
import { readProjectPresentations } from "./projectPresentationPlugins";
import type { WorkspaceRpc } from "./workspaceRpc";

export type PresentationScenario = {
    snapshot: PresentationSnapshot;
    expanded?: NodeAddress[];
    inputs: Timed<PresentationInput>[];
};

export async function runPresentationScenario(project: string, rpc: WorkspaceRpc, interactions: NodeEventInteractions,
    owner: string, { snapshot, expanded, inputs }: PresentationScenario) {
    const { address, definitionId } = snapshot.node;
    const allowEdits = snapshot.editable;
    if (!address || !Array.isArray(inputs) || inputs.length > 8192
        || inputs.some(({ atMs }, i) => !Number.isFinite(atMs) || atMs < 0 || atMs > 20_000
            || (i > 0 && inputs[i - 1].atMs > atMs))) throw new Error("Invalid presentation scenario");
    const programs = projectProgramsByDefinition(await readProjectPresentations(project, (error) => console.error(error)));
    const program = programs.get(definitionId) ?? builtinPresentationPrograms.get(definitionId);
    if (!program) throw new Error(`No presentation program for ${definitionId}`);
    const present = "create" in program ? program.create() : (() => {
        // Use the same JSON-shaped DTO that the web worker receives. A
        // scenario runs trusted project code and uses a separate JavaScript
        // context so that built-in and project programs have the same API.
        const context = createContext(Object.create(null));
        new Script(program.source).runInContext(context, { timeout: 1500 });
        const frame = new Script("JSON.stringify(present(...JSON.parse(input)))");
        return (snapshot: PresentationSnapshot, inputs: readonly PresentationInput[]) => {
            context.input = JSON.stringify([snapshot, inputs]);
            return JSON.parse(frame.runInContext(context, { timeout: 1500 }));
        };
    })();

    let acceptedRevision = 0;
    let failure: unknown;
    let snapshotReads = Promise.resolve();
    const frames: PresentationFrame[] = [];
    let readCount = 0;
    const readSnapshot = async (minimumRevision: number, timeoutMs: number) => {
        const deadline = performance.now() + timeoutMs;
        for (;;) {
            const scope = graphScopeWithChildren(await rpc.getGraphScope(expanded));
            const target = scope.nodes.find((node) => addressKey(node.address) === addressKey(address));
            const revision = scope.sessionRevision ?? snapshot.revision;
            const prepared = !scope.execution || scope.generation === scope.execution.preparedGeneration
                || scope.generation === scope.execution.activeGeneration;
            if (target?.loadState === "ready" && scope.execution?.phase !== "configurationFailed"
                && prepared && revision >= minimumRevision) {
                if (target.definitionId !== definitionId) throw new Error("Scenario definition differs from the active node");
                snapshot = { ...snapshot, revision, editable: allowEdits && snapshot.mode === "editable" && scope.editable,
                    node: { ...snapshot.node, arguments: target.construction?.arguments ?? [] } };
                return;
            }
            if (performance.now() >= deadline) throw new Error(scope.execution?.message || "The presentation node is unavailable");
            await delay(100);
        }
    };
    const notifySnapshot = () => session.update({ ...snapshot,
        editsPending: edits.queue.pending || snapshot.revision < acceptedRevision });
    const nodeEvents = new NodeEventMessaging((request) => {
        void answerNodeEventRequest(interactions, owner, request).then((reply) => nodeEvents.receive(reply));
    }, randomUUID);
    const edits = new PresentationLaneEdits({
        editable: () => snapshot.editable,
        gesture: () => nodeEvents.gesture(address),
        input: (input) => session.input(input),
        setError: (editError) => { snapshot = { ...snapshot, editError }; },
        fail: (error) => { failure = error; },
        changed: notifySnapshot,
        accepted: (revision) => {
            acceptedRevision = Math.max(acceptedRevision, revision);
            ++readCount;
            snapshotReads = snapshotReads.then(async () => {
                await readSnapshot(revision, 10_000);
                notifySnapshot();
            }).catch((error) => { failure = error; }).finally(() => { --readCount; });
        },
    });
    let scheduled = false;
    const wake = new PresentationWake(() => session.input({ type: "wake" }));
    const session = new PresentationPluginSession(present, () => {
        if (scheduled) return;
        scheduled = true;
        setTimeout(() => { scheduled = false; void session.flush(); }, 16);
    }, (frame, draw, inputs) => {
        if (draw) {
            if (frames.length >= 8192) throw new Error("The presentation scenario produced too many frames");
            frames.push(frame);
        }
        if (!inputs.length && !wake.pending) return;
        edits.accept(frame, (state) => {
            if (snapshot.state === state) return;
            snapshot = { ...snapshot, state };
            notifySnapshot();
        });
        if (frame.wakeAfterMs !== undefined) wake.request(frame.wakeAfterMs);
    }, (error) => { failure = error; });
    try {
        await readSnapshot(0, 60_000);
        session.update(snapshot, "immediate");
        const started = performance.now();
        for (const { atMs, value } of inputs) {
            const remaining = started + atMs - performance.now();
            if (remaining > 0) await delay(remaining);
            if (failure) throw failure;
            session.input(value);
        }
        const deadline = performance.now() + 15_000;
        while (!failure && (edits.queue.pending || session.pending || readCount || wake.pending)) {
            if (performance.now() >= deadline) throw new Error("The presentation scenario did not settle");
            await delay(5);
        }
        if (failure) throw failure;
        return { presentation: program.manifest.id, frames, snapshot, acceptedRevision };
    } finally {
        wake.cancel();
        session.dispose();
        edits.queue.dispose();
        await edits.closeGestures();
        interactions.releaseOwner(owner);
        nodeEvents.disconnected();
    }
}
