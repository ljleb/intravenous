import assert from "node:assert/strict";
import Module from "node:module";
import { test } from "node:test";
import type * as vscode from "vscode";
import { SetBeatGrid } from "../src/generated/beatsEvents";
import type { GraphEditorHostMessage, GraphEditorWebviewMessage } from "../src/graphEditorProtocol";
import { NodeEventInteractions } from "../src/nodeEventInteractions";
import { serializeNodeEvent } from "../src/nodeEvents";
import type { WorkspaceSession } from "../src/workspaceSession";

function graphPanel(active: boolean, visible = true) {
    type Receive = (message: GraphEditorWebviewMessage) => Promise<void>;
    let receive!: Receive;
    let visibilityChanged!: () => void;
    let disposed!: () => void;
    const posted: GraphEditorHostMessage[] = [];
    const disposable = { dispose() {} };
    const panel = {
        active, visible, viewColumn: 1,
        reveal() { panel.visible = true; visibilityChanged(); },
        onDidChangeViewState(listener: () => void) { visibilityChanged = listener; return disposable; },
        onDidDispose(listener: () => void) { disposed = listener; return disposable; },
        dispose() { disposed(); },
        webview: {
            onDidReceiveMessage(listener: Receive) { receive = listener; return disposable; },
            async postMessage(message: GraphEditorHostMessage) { posted.push(message); return true; },
            asWebviewUri: (uri: string) => uri,
        },
    };
    return { panel: panel as unknown as vscode.WebviewPanel, posted,
        hide() { panel.visible = false; visibilityChanged(); },
        receive: (message: GraphEditorWebviewMessage) => receive(message),
        request() {
            const message = posted.filter(({ type }) => type === "interruptProjectHistory").at(-1);
            assert.equal(message?.type, "interruptProjectHistory");
            return message.requestId;
        },
        acknowledge(requestId: number) { return receive({ type: "projectHistoryInterrupted", requestId }); },
    };
}

test("project history waits for cleanup acknowledgments and prevents stale node edits", async (t) => {
    const disposable = { dispose() {} };
    const event = () => disposable;
    const loader = Module as typeof Module & {
        _load(request: string, parent: NodeModule, isMain: boolean): unknown;
    };
    const load = loader._load;
    const stub = t.mock.method(loader, "_load", function (this: typeof loader,
        request: string, parent: NodeModule, isMain: boolean) {
        if (request !== "vscode") return load.call(this, request, parent, isMain);
        return { Uri: { joinPath: (...segments: string[]) => segments.join("/") },
            Disposable: { from: event }, StatusBarAlignment: { Left: 0 },
            window: { state: { focused: false }, onDidChangeWindowState: event,
                createStatusBarItem: () => ({ ...disposable, hide() {} }) },
            workspace: { onDidGrantWorkspaceTrust: event, onDidChangeConfiguration: event } };
    });
    let Provider: typeof import("../src/graphEditorProvider").GraphEditorProvider;
    try { ({ GraphEditorProvider: Provider } = require("../src/graphEditorProvider")); }
    finally { stub.mock.restore(); }

    const calls: string[] = [];
    let acceptEdit!: () => void;
    const edit = new Promise<void>((resolve) => { acceptEdit = resolve; });
    const nodeEvents = new NodeEventInteractions({
        begin: async () => { calls.push("begin"); },
        send: async () => { await edit; calls.push("send"); },
        end: async () => { calls.push("end"); },
        report: (error) => assert.fail(String(error)),
    });
    const active = graphPanel(true), other = graphPanel(false), hidden = graphPanel(false, false);
    const editor = new Provider("extension" as unknown as vscode.Uri, {
        graphSessionStatus: "starting", nodeEvents, projectHistory: async (direction: string) => { calls.push(direction); },
        onDidChangeGraph: event, onDidChangePresentations: event, onDidChangeManualControls: event, onDidChangeObservations: event,
        manualControls: { releaseOwner() {}, states: () => [], live: () => [] }, reconcileObservations: async () => {},
    } as unknown as WorkspaceSession, {} as vscode.OutputChannel);
    t.after(() => { acceptEdit(); editor.dispose(); });
    for (const view of [active, other, hidden]) {
        editor.revive(view.panel);
        await view.receive({ type: "graphReady" });
    }

    const target = { instanceId: "beats" };
    const preview = serializeNodeEvent(SetBeatGrid({ bpm: 133, beats_per_bar: 4, subdivisions: [2] }));
    await active.receive({ type: "nodeEventGesture", requestId: "begin", target, begin: true, interactionId: "drag" });
    const pending = active.receive({ type: "sendNodeEvent", requestId: "previous", target, event: preview, interactionId: "drag" });
    const undo = editor.runAction("undo");
    const turn = () => new Promise<void>((resolve) => setImmediate(resolve));
    await turn();
    const undoId = active.request();
    assert.equal(other.request(), undoId);
    assert.deepEqual(calls, ["begin"]);
    await active.acknowledge(undoId);
    await other.acknowledge(undoId + 1);
    const late = { type: "sendNodeEvent", requestId: "late", target, event: preview } as const;
    await other.receive(late);
    assert.deepEqual(other.posted.at(-1), { type: "nodeEventResult", requestId: "late", error: "Project history interrupted the node edit" });
    await other.acknowledge(undoId);
    await turn();
    assert.deepEqual(calls, ["begin"]);
    acceptEdit();
    await pending;
    await undo;
    assert.deepEqual(calls, ["begin", "send", "end", "undo"]);

    // A suspended panel does not delay history, but remains gated on reveal.
    hidden.panel.reveal();
    assert.equal(hidden.request(), undoId);
    await hidden.receive(late);
    assert.deepEqual(hidden.posted.at(-1), other.posted.filter((message) => message.type === "nodeEventResult").at(-1));
    await hidden.acknowledge(undoId);
    assert.deepEqual(hidden.posted.at(-1), { type: "projectHistoryFinished", requestId: undoId });

    // The keyboard path uses the same handshake, including its initiating panel.
    const redo = active.receive({ type: "projectHistory", direction: "redo" });
    await turn();
    const redoId = active.request();
    assert.ok(redoId > undoId);
    assert.equal(other.request(), redoId);
    other.hide();
    await active.acknowledge(undoId);
    hidden.panel.dispose();
    await turn();
    assert.equal(calls.at(-1), "undo");
    await active.acknowledge(redoId);
    await redo;
    assert.equal(calls.at(-1), "redo");
    assert.deepEqual(active.posted.at(-1), { type: "projectHistoryFinished", requestId: redoId });

    other.panel.reveal();
    await other.receive(late);
    assert.deepEqual(other.posted.at(-1), { type: "nodeEventResult", requestId: "late", error: "Project history interrupted the node edit" });
    await other.acknowledge(redoId);
    assert.deepEqual(other.posted.at(-1), { type: "projectHistoryFinished", requestId: redoId });
});
