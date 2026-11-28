import assert from "node:assert/strict";
import Module from "node:module";
import { test, type TestContext } from "node:test";
import type * as vscode from "vscode";
import type { GraphScope, GraphScopeNode } from "../src/graphScope";
import type { WorkspaceSession } from "../src/workspaceSession";
import type { GraphEditorProvider } from "../src/graphEditorProvider";

function loadWorkspaceClasses(t: TestContext) {
    const loader = Module as typeof Module & { _load(request: string, parent: NodeModule, isMain: boolean): unknown };
    const load = loader._load;
    const vscode = {
        ThemeColor: class {},
        MarkdownString: class {
            value = "";
            appendText(text: string) { this.value += text; return this; }
        },
    };
    const stub = t.mock.method(loader, "_load", function (this: typeof loader, request: string, parent: NodeModule, isMain: boolean) {
        return request === "vscode" ? vscode : load.call(this, request, parent, isMain);
    });
    try {
        return { Session: require("../src/workspaceSession").WorkspaceSession as typeof WorkspaceSession,
            Provider: require("../src/graphEditorProvider").GraphEditorProvider as typeof GraphEditorProvider };
    } finally { stub.mock.restore(); }
}

function deferred<T>() {
    let finish!: (value: T) => void;
    const result = new Promise<T>((resolve) => { finish = resolve; });
    return { result, finish };
}

test("a late instance reply cannot restore the previous source file", async (t) => {
    const { Session } = loadWorkspaceClasses(t);
    const sourceA = deferred<{ instances: object[] }>();
    const queried: string[] = [];
    const committed: string[] = [];
    const session = Object.assign(Object.create(Session.prototype), {
        rpc: {
            getNodeInstances: (path: string) => path === "/a.cpp" ? sourceA.result : Promise.resolve({ instances: [{ instanceId: path }] }),
            queryNodesBySpans: async (path: string) => { queried.push(path); return { nodes: [] }; },
            queryActiveRegions: async () => ({ sourceSpans: [] }),
        },
        sourceQueryRequest: 0,
        provider: { setPackageRoot() {}, setNodes() {} },
        highlighter: { clearPrimary() {}, setActiveRegions() {} },
        refreshVisibleInstances(this: { ivModuleInstances: { instanceId: string }[] }) {
            committed.push(this.ivModuleInstances[0].instanceId);
        },
    }) as WorkspaceSession;
    const editor = (fsPath: string) => ({ document: { uri: { scheme: "file", fsPath } },
        selections: [{ start: { line: 0, character: 0 }, end: { line: 0, character: 1 } }] }) as unknown as vscode.TextEditor;
    const pending = session.updateFromEditor(editor("/a.cpp"));
    await session.updateFromEditor(editor("/b.cpp"));
    sourceA.finish({ instances: [{ instanceId: "/a.cpp" }] });
    await pending;
    assert.deepEqual(committed, ["/b.cpp"]);
    assert.deepEqual(queried, ["/b.cpp"]);
});

test("an in-flight graph scope satisfies a later invalidation at its revision", async (t) => {
    const { Provider } = loadWorkspaceClasses(t);
    let reads = 0;
    const scope = deferred<GraphScope>();
    const session = { graphSessionStatus: "running", graphRevision: 4, graphConnectionNumber: 1,
        readGraphScope: () => { ++reads; return scope.result; },
        graphDefinitions: async () => ({}), manualControls: { reconcile() {} } };
    const errors: string[] = [];
    const state: { scope: GraphScope | null } = { scope: null };
    const provider = Object.assign(Object.create(Provider.prototype) as { refresh(): Promise<void> }, {
        session, panels: new Set([{}]), expandedByPanel: new Map(), graphUiProfiles: new Map(), state, post() {},
        graphErrorStatus: { hide() {} },
        outputChannel: { appendLine(message: string) { errors.push(message); } },
    });
    const first = provider.refresh();
    await new Promise<void>((resolve) => setImmediate(resolve));
    session.graphRevision = 5;
    const second = provider.refresh();
    scope.finish({ sessionRevision: 5, generation: 1, editable: true, nodes: [], connections: [] });
    await Promise.all([first, second, provider.refresh()]);
    assert.equal(reads, 1);
    assert.equal(state.scope?.sessionRevision, 5);
    assert.deepEqual(errors, []);
});

test("the graph error status waits for definitions and reports confirmed failures", async (t) => {
    const { Provider } = loadWorkspaceClasses(t);
    let visible = false;
    const status = { text: "", tooltip: { value: "" },
        show() { visible = true; }, hide() { visible = false; } };
    const provider = Object.assign(Object.create(Provider.prototype) as { updateGraphErrorStatus(scope: GraphScope): void }, {
        graphErrorStatus: status,
    });
    const node: GraphScopeNode = {
        loadState: "waitingForDefinition", address: { instanceId: "clip" },
        instance: { instanceId: "clip", definitionId: "builtin.clip", realized: false },
        sampleInputs: [], sampleOutputs: [], eventInputs: [], eventOutputs: [], hasChildren: false,
        errorMessage: "The definition is unavailable in this snapshot.",
    };
    const execution = { phase: "configurationFailed", message: "", preparedGeneration: null, activeGeneration: null };
    const scope: GraphScope = { generation: null, editable: true, nodes: [node], connections: [], execution };

    provider.updateGraphErrorStatus(scope);
    assert.equal(visible, false);
    node.loadState = "waitingForGraph";
    execution.phase = "compiling";
    provider.updateGraphErrorStatus(scope);
    assert.equal(visible, false);

    execution.phase = "failed";
    execution.message = "Feedback requires a delay.";
    provider.updateGraphErrorStatus(scope);
    assert.equal(visible, true);
    assert.match(status.text, /graph compilation failed/);
    assert.ok(status.tooltip.value.startsWith(execution.message));

    execution.phase = "configurationFailed";
    scope.nodes.push({ ...node, address: { instanceId: "saw" },
        instance: { instanceId: "saw", definitionId: "iv.project.saw", realized: false },
        loadState: "waitingForDefinition" });
    node.loadState = "failed";
    node.errorMessage = "The built package did not publish the requested definition.";
    provider.updateGraphErrorStatus(scope);
    assert.equal(visible, true);
    assert.match(status.text, /graph configuration failed/);
    assert.ok(status.tooltip.value.startsWith(node.errorMessage));
});
