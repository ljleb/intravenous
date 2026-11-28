import assert from "node:assert/strict";
import test from "node:test";
import { setImmediate as nextTurn } from "node:timers/promises";
import type { GraphScope } from "../src/graphScope";
import { ManualControlInteractions, type ManualControlId } from "../src/manualControls";
import type { JsonRpcSocketClient } from "../src/rpcClient";
import { WorkspaceRpc } from "../src/workspaceRpc";

const seeds = process.env.IV_KNOB_STRESS_SEED === undefined ? [1, 17, 1849, 33064]
    : [Number(process.env.IV_KNOB_STRESS_SEED)];
type RequestParams = { node?: ManualControlId["node"]; expectedRevision?: number;
    interactionId?: string; value?: number; enabled?: boolean };

for (const seed of seeds) test(`rapid knob edits preserve value and enable state (seed ${seed})`, async () => {
    assert.ok(Number.isInteger(seed) && seed >= 0 && seed <= 0xffffffff, "Expected a uint32 stress seed");
    let randomState = seed >>> 0;
    const choose = (count: number) => {
        randomState = (Math.imul(randomState, 1664525) + 1013904223) >>> 0;
        return Math.floor(randomState / 2 ** 32 * count);
    };
    const expected = [{ value: .25, enabled: true }, { value: .5, enabled: false }];
    const actual = expected.map((state) => ({ ...state }));
    const controls: ManualControlId[] = [0, 1].map((index) => ({
        node: { instanceId: String(index) }, inputName: "gain",
    }));
    let revision = 1849;
    const pendingGestures = new Map<string, number>();
    const errors: unknown[] = [];
    const trace: string[] = [];
    let queue = Promise.resolve();
    const rpc = new WorkspaceRpc({
        request(method: string, params: RequestParams) {
            const call = queue.then(async () => {
                // The server can receive more UI events before the previous
                // request or its revision notification reaches the editor.
                await nextTurn();
                if (params.expectedRevision !== undefined && params.expectedRevision !== revision)
                    throw new Error(`stale project session revision: expected ${params.expectedRevision}, current ${revision}`);
                const index = Number(params.node?.instanceId);
                switch (method) {
                    case "manualControls.toggleInput":
                        assert.equal(params.value, undefined);
                        assert.equal(params.enabled, undefined);
                        actual[index].enabled = !actual[index].enabled;
                        break;
                    case "manualControls.begin":
                        assert.ok(![...pendingGestures.values()].includes(index));
                        pendingGestures.set(params.interactionId, index);
                        break;
                    case "manualControls.end": {
                        const target = pendingGestures.get(params.interactionId);
                        assert.notEqual(target, undefined, "release without an accepted press");
                        actual[target!].value = params.value;
                        pendingGestures.delete(params.interactionId);
                        break;
                    }
                    case "manualControls.hover": return;
                    default: assert.fail(`Unexpected request ${method}`);
                }
                ++revision;
            });
            queue = call.catch((error) => { errors.push(error); });
            return call;
        },
        notify(method: string, params: RequestParams) {
            assert.equal(method, "manualControls.update");
            assert.ok(pendingGestures.has(params.interactionId));
        },
    } as unknown as JsonRpcSocketClient, () => revision);
    const interactions = new ManualControlInteractions(() => rpc, () => {});
    const refresh = () => interactions.reconcile({ generation: 1, sessionRevision: revision, editable: true,
        connections: [], nodes: controls.map(({ node }, index) => ({ address: node, loadState: "ready",
            instance: { instanceId: node.instanceId, definitionId: "gain", realized: true },
            sampleInputs: [{ name: "gain", index: 0, manualControl: { ...actual[index], available: true,
                participates: actual[index].enabled, participation: "disconnectedOnly" } }],
            sampleOutputs: [], eventInputs: [], eventOutputs: [], hasChildren: false,
        })) } as GraphScope);
    refresh();

    const open: Array<string | undefined> = [undefined, undefined];
    const end = (index: number, value: number) => {
        interactions.action(String(index), { action: "end", interactionId: open[index]!, value });
        open[index] = undefined;
        expected[index].value = value;
    };
    let overlap = 0;
    for (let step = 0; step < 180; ++step) {
        const index = choose(controls.length), action = choose(6), value = (step + 1) / 256;
        const control = controls[index], owner = String(index);
        trace.push(`${step} knob=${owner} action=${action} pressed=${!!open[index]}`);
        if (action < 2) {
            const clicks = 1 + choose(4);
            trace.push(`  clicks=${clicks}`);
            for (let click = 0; click < clicks; ++click) {
                void rpc.toggleManualInput(control).catch(() => {}); // Collected by the simulated transport.
                expected[index].enabled = !expected[index].enabled;
                if (open[index]) ++overlap;
            }
        } else if (action === 5) {
            interactions.action(owner, { action: "hover", control: choose(2) === 0 ? control : null });
        } else if (!open[index]) {
            const interactionId = String(step);
            interactions.action(owner, { action: "begin", interactionId, control, value });
            open[index] = interactionId;
        } else if (action === 2) {
            interactions.action(owner, { action: "update", interactionId: open[index]!, value });
        } else end(index, value);
        if (choose(4) === 0) await nextTurn();
        if (choose(5) === 0) { ++revision; refresh(); }
    }
    for (const [index, interaction] of open.entries()) if (interaction) end(index, .875);
    await interactions.settled();
    await queue;
    const history = trace.slice(-24).join("\n");
    assert.ok(overlap > 0, history);
    assert.deepEqual(errors, [], history);
    for (const { node } of controls)
        assert.ok(interactions.states(node.instanceId).every(({ phase }) => phase === "settled"), history);
    assert.equal(pendingGestures.size, 0, history);
    assert.deepEqual(actual, expected, history);
});
