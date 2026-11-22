import { randomUUID } from "node:crypto";
import { setTimeout as delay } from "node:timers/promises";
import type { Timed } from "./clientInteractionTrace";
import { addressKey, graphScopeWithChildren, type NodeAddress } from "./graphScope";
import { NodeEventMessaging, answerNodeEventRequest } from "./nodeEventMessaging";
import type { NodeEventInteractions } from "./nodeEventInteractions";
import { PresentationLaneEdits } from "./presentationLaneEdits";
import type { PresentationFrame, PresentationInput, PresentationSnapshot } from "./presentationPlugin";
import { PresentationPluginSession, PresentationWake } from "./presentationPluginSession";
import { renderPresentationTimelineGrid } from "./presentationTimelineGrid";
import { builtinPresentationPrograms, projectProgramsByDefinition } from "./presentationPrograms";
import { PresentationArtifactReader } from "./presentationArtifacts";
import { createNodePresentationWorker } from "./nodeWasmPresentationWorker";
import type { WorkspaceRpc } from "./workspaceRpc";

export type PresentationScenario = {
    snapshot: PresentationSnapshot;
    presentationId?: string;
    expanded?: NodeAddress[];
    inputs: Timed<PresentationInput>[];
};

export async function runPresentationScenario(rpc: WorkspaceRpc, interactions: NodeEventInteractions,
    owner: string, { snapshot, presentationId, expanded, inputs }: PresentationScenario) {
    const { address, definitionId } = snapshot.node;
    const allowEdits = snapshot.editable;
    if (!address || !Array.isArray(inputs) || inputs.length > 8192
        || inputs.some(({ atMs }, i) => !Number.isFinite(atMs) || atMs < 0 || atMs > 20_000
            || (i > 0 && inputs[i - 1].atMs > atMs))) throw new Error("Invalid presentation scenario");
    const reader = new PresentationArtifactReader();
    const report = (error: string) => console.error(error);
    const deadline = performance.now() + 60_000;
    let catalog = await reader.read(rpc, report);
    while (!catalog.ready) {
        if (performance.now() >= deadline) throw new Error("The presentation catalog did not become ready");
        await delay(50);
        catalog = await reader.read(rpc, report);
    }
    const program = presentationId
        ? catalog.artifacts.find((entry) => entry.id === presentationId && entry.manifest.definitionIds.includes(definitionId))
        : projectProgramsByDefinition(catalog.artifacts).get(definitionId) ?? builtinPresentationPrograms.get(definitionId);
    if (!program) throw new Error(`No presentation program for ${definitionId}`);
    const worker = "bytes" in program ? createNodePresentationWorker(program.bytes) : undefined;
    worker?.restore(snapshot.state);
    const present = "create" in program ? program.create() : worker!.run.bind(worker);

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
            const { time, width, height } = snapshot.viewport;
            const grid = frame.timelineGrid && time
                ? renderPresentationTimelineGrid(frame.timelineGrid, time, width, height, snapshot.mode === "editable") : undefined;
            frames.push(grid ? { ...frame, marks: [...grid.marks, ...frame.marks ?? []] } : frame);
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
        return { presentation: "id" in program ? program.id : program.manifest.id, frames, snapshot, acceptedRevision };
    } finally {
        wake.cancel();
        session.dispose();
        worker?.stop();
        edits.queue.dispose();
        await edits.closeGestures();
        interactions.releaseOwner(owner);
        nodeEvents.disconnected();
    }
}
