import assert from "node:assert/strict";
import { Duplex } from "node:stream";
import test from "node:test";
import { canContinuePortConnection, connectionEndsAtPort, type GraphPortEndpoint } from "../src/graphConnectionEditing";
import { graphScopeWithChildren, type GraphPortMatcher, type GraphScope, type GraphScopeConnection,
    type GraphScopeNode, type NodeAddress } from "../src/graphScope";
import { beginPortConnection, finishPortConnection, movePortConnection, portConnectionDrop } from "../src/portConnectionGesture";
import { JsonRpcSocketClient, StaleProjectRevisionError } from "../src/rpcClient";
import { WorkspaceRpc } from "../src/workspaceRpc";

const seeds = process.env.IV_PORT_STRESS_SEED === undefined ? [1, 17, 1849, 33064]
    : [Number(process.env.IV_PORT_STRESS_SEED)];
const drops = ["gutter", "column hover", "canvas", "wrong direction", "wrong kind", "rewire", "original",
    "group button", "knob", "outside", "wrong pointer", "click"] as const;
const races = ["none", "before read", "during write", "selected wire changed", "busy", "write error"] as const;

// GraphEditor obtains these relationships from elementFromPoint. Model just
// the selectors used by the release handler, including the empty column area.
class Hit {
    readonly children: Hit[] = [];
    readonly dataset: { portEndpoint?: string } = {};
    constructor(readonly selectors: string[] = [], readonly parent?: Hit, endpoint?: GraphPortEndpoint) {
        parent?.children.push(this);
        if (endpoint) this.dataset.portEndpoint = JSON.stringify(endpoint);
    }
    closest(query: string): Hit | null {
        return query.split(/,\s*/).some((selector) => this.selectors.includes(selector)) ? this : this.parent?.closest(query) ?? null;
    }
    querySelector(selector: string): Hit | null {
        return this.children.find((child) => child.selectors.includes(selector)) ?? null;
    }
    contains(hit: Hit): boolean { return hit === this || this.children.some((child) => child.contains(hit)); }
    element(): Element { return this as unknown as Element; }
}

function scenario(pick: (limit: number) => number) {
    const kind = pick(2) ? "sample" : "event";
    const direction = pick(2) ? "input" : "output";
    const side = direction === "input" ? "inputs" : "outputs";
    const otherSide = direction === "input" ? "outputs" : "inputs";
    const boundary = pick(2) === 0;
    const nested = pick(2) === 0;
    const wildcard = pick(2) === 0;
    const extraEnd = pick(2) === 0;
    const drop = drops[pick(drops.length)];
    const race = races[pick(races.length)];
    const hasType = drop === "rewire" || pick(2) === 0;
    const channel = kind === "sample" && pick(2) === 0 ? 0 : undefined;
    const address: NodeAddress = { instanceId: "module", ...(nested ? { path: [{ kind: "tiledChild", name: "left" }] } : {}) };
    const chosen: GraphPortEndpoint = { address, kind, direction, name: "main", index: 0, moduleBoundary: boundary };
    const sibling = { ...chosen, name: "side", index: 1 };
    const next = { ...chosen, address: { instanceId: "next" }, moduleBoundary: false };
    const type = kind === "sample" ? "mono" : "trigger";
    const authored = (port: GraphPortEndpoint, selectAll = false, channelIndex = channel): GraphPortMatcher => ({
        ...port.address, path: port.address.path ?? [], ...(port.moduleBoundary ? { moduleBoundary: true } : {}),
        port: { ...(selectAll ? {} : { name: port.name, index: port.index }),
            ...(channelIndex === undefined ? {} : { channel: channelIndex }) },
    });
    const node = (address: NodeAddress, hasType: boolean): GraphScopeNode => {
        const ports = ["main", "side"].map((name, index) => ({ name, index,
            channelType: hasType ? "mono" : undefined, eventType: hasType ? "trigger" : undefined }));
        return { address, instance: { instanceId: address.instanceId, definitionId: "test.node", realized: true },
            loadState: "ready", hasChildren: false, sampleInputs: ports, sampleOutputs: ports, eventInputs: ports, eventOutputs: ports };
    };
    const fixed = { instanceId: "fixed", port: { name: "main", index: 0 } };
    const extra = { instanceId: "extra", port: { name: "main", index: 0 } };
    const movingSide = [authored(chosen, wildcard), ...(extraEnd ? [extra] : [])];
    const wire: GraphScopeConnection = { id: "picked", applied: hasType,
        connection: { connectionId: "picked", kind, sourceType: type, targetType: type,
            [side]: movingSide, [otherSide]: [fixed] } as GraphScopeConnection["connection"] };
    const unrelated: GraphScopeConnection = { ...wire, id: "other", connection: { ...wire.connection, connectionId: "other" } };
    const scope: GraphScope = { generation: 1, editable: true, connections: [unrelated, wire],
        nodes: [node(fixed, true), node(extra, true), node(next.address, true), ...(!nested ? [node(address, hasType)] : [])],
        ...(nested ? { childScopes: [{ subject: { instanceId: "module" }, nodes: [node(address, hasType)] }] } : {}),
    };
    const surface = new Hit();
    const group = new Hit(["[data-port-group]"], surface);
    const endpointHit = (port: GraphPortEndpoint) => {
        const ring = new Hit(["button", "[data-port-endpoint]"], group, port);
        return new Hit([], ring);
    };
    const hit = {
        "gutter": group,
        "column hover": new Hit(["[data-port-control]", ".port-column-hover"], group),
        "canvas": surface,
        "wrong direction": endpointHit({ ...next, direction: direction === "input" ? "output" : "input" }),
        "wrong kind": endpointHit({ ...next, kind: kind === "sample" ? "event" : "sample" }),
        "rewire": endpointHit(next),
        "original": endpointHit(chosen),
        "group button": new Hit(["button"], group),
        "knob": new Hit([], new Hit(["[data-port-control]:not(.port-column-hover)"], group)),
        "outside": new Hit(),
        "wrong pointer": surface,
        "click": surface,
    }[drop];
    const shouldEdit = !["original", "group button", "knob", "outside", "wrong pointer", "click"].includes(drop);
    const retained = [ ...(drop === "rewire" ? [authored(next)] : []), ...(wildcard ? [authored(sibling)] : []), ...(extraEnd ? [extra] : []) ];
    const expected = !shouldEdit ? null : retained.length
        ? { method: "graphConnections.upsert", params: { connection: { ...wire.connection, [side]: retained } } }
        : { method: "graphConnections.delete", params: { connectionId: wire.id } };
    return { scope, chosen, wire, unrelated, next, drop, race, hit, surface, expected, hasType, side, movingSide, extra, wildcard, nested, boundary };
}

for (const seed of seeds) test(`randomized port disconnects and revision races (seed ${seed})`, async (t) => {
    assert.ok(Number.isInteger(seed) && seed >= 0 && seed <= 0xffffffff, "Expected a uint32 stress seed");
    let state = seed, currentRevision = 100, clientRevision = 100;
    const pick = (limit: number) => {
        state = (Math.imul(state, 1664525) + 1013904223) >>> 0;
        return Math.floor(state / 2 ** 32 * limit);
    };
    let model!: ReturnType<typeof scenario>;
    let scope!: GraphScope;
    let otherWire!: GraphScopeConnection;
    let reads = 0, writes = 0;
    let applied: { method: string; params: Record<string, unknown> } | undefined;
    const socket = new Duplex({ read() {}, write(bytes, _encoding, done) {
        const { id, method, params } = JSON.parse(String(bytes));
        let reply;
        if (method === "graph.getScope") {
            if (++reads === 1 && model.race === "before read") ++currentRevision;
            reply = { result: { ...scope, sessionRevision: currentRevision } };
        } else {
            ++writes;
            if (model.race === "busy" || (model.race === "during write" && writes <= 2)) ++currentRevision;
            if (model.race === "during write" && writes === 1) {
                otherWire = { ...model.unrelated, connection: { ...model.unrelated.connection,
                    [model.side]: [model.extra] } };
                scope.connections[0] = otherWire;
            }
            if (model.race === "selected wire changed" && writes === 1) {
                scope.connections[1] = { ...model.wire, connection: { ...model.wire.connection,
                    [model.side]: [...model.movingSide, model.extra, { instanceId: "unloaded", port: { index: 0 } }] } };
                ++currentRevision;
            }
            const { expectedRevision, ...editParams } = params;
            if (expectedRevision !== currentRevision) {
                reply = { error: { code: -32009, message: "stale project session revision", data: { currentRevision } } };
            } else if (model.race === "write error") {
                reply = { error: { code: -32000, message: "cannot update connection", data: { currentRevision } } };
            } else {
                assert.equal(applied, undefined, "A release can apply at most one write");
                applied = { method, params: editParams };
                if (method === "graphConnections.delete") scope.connections = scope.connections.filter((entry) => entry.id !== editParams.connectionId);
                else scope.connections[1] = { ...model.wire, connection: editParams.connection };
                reply = { result: { sessionRevision: ++currentRevision } };
            }
        }
        queueMicrotask(() => socket.push(JSON.stringify({ jsonrpc: "2.0", id, ...reply }) + "\n"));
        done();
    } });
    const client = new JsonRpcSocketClient(socket, null, null, (revision) => { clientRevision = revision; });
    await client.connect();
    t.after(() => client.dispose());
    const rpc = new WorkspaceRpc(client, () => clientRevision);
    const exercised = new Set<string>();
    for (let step = 0; step < 192; ++step) {
        model = scenario(pick);
        scope = structuredClone(model.scope);
        otherWire = model.unrelated;
        reads = writes = 0;
        applied = undefined;
        const { chosen, next, drop, race, hit, surface, expected } = model;
        const trace = `seed=${seed} step=${step} ${chosen.kind}/${chosen.direction} ${drop}, ${race}, `
            + `typed=${model.hasType} boundary=${model.boundary} nested=${model.nested} wildcard=${model.wildcard}`;
        try {
            const graph = graphScopeWithChildren(scope);
            const end = connectionEndsAtPort(graph, chosen).find(({ connection }) => connection.id === model.wire.id);
            assert.ok(end);
            assert.equal(canContinuePortConnection(graph, chosen, end), true);
            const pickup = { x: 10, y: 10 };
            const release = drop === "click" ? pickup : { x: 80, y: 60 };
            let drag = beginPortConnection(chosen, 1, 0, pickup, end)!;
            for (let i = pick(4); i > 0; --i)
                drag = movePortConnection(drag, pick(2) + 1, 1, release)!;
            const destination = portConnectionDrop(hit.element(), surface.element());
            const result = finishPortConnection(graph, drag, drop === "wrong pointer" ? 2 : 1, release,
                destination.target, destination.emptyEditorSpace);
            if (!expected) {
                assert.equal(result, null);
                assert.deepEqual(scope.connections, model.scope.connections);
                continue;
            }
            assert.ok(result?.type === "edit");
            assert.deepEqual(result.edit.target, drop === "rewire" ? next : null);
            exercised.add(drop);
            exercised.add(race);
            const apply = rpc.editGraphConnectionEnd(result.edit, model.nested ? [{ instanceId: "module" }] : []);
            if (race === "selected wire changed") {
                await assert.rejects(apply, /selected connection changed/);
                assert.equal(reads, 2);
                assert.equal(writes, 1);
                assert.equal(applied, undefined);
                assert.equal(scope.connections[1].connection[model.side].at(-1)?.instanceId, "unloaded");
            } else if (race === "busy" || race === "write error") {
                await assert.rejects(apply, race === "busy" ? StaleProjectRevisionError : /cannot update connection/);
                assert.equal(writes, race === "busy" ? 4 : 1);
                assert.equal(applied, undefined);
            } else {
                await apply;
                assert.deepEqual(applied, expected);
                assert.equal(reads, race === "during write" ? 3 : 1);
                assert.equal(writes, reads);
            }
            assert.deepEqual(scope.connections[0], otherWire);
        } catch (error) {
            throw new Error(trace, { cause: error });
        }
    }
    for (const choice of ["gutter", "column hover", "wrong direction", "wrong kind", "rewire", ...races])
        assert.ok(exercised.has(choice), `seed=${seed} did not exercise ${choice}`);
});
