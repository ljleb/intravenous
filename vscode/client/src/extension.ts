import "reflect-metadata";

import type { ChildProcess } from "node:child_process";
import { existsSync } from "node:fs";
import * as vscode from "vscode";
import { container } from "tsyringe";

import { LiveGraphViewProvider } from "./liveGraphViewProvider";
import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { WorkspaceSessionFactory } from "./workspaceSessionFactory";
import { timestampOutput } from "./outputLog";
import { GraphEditorProvider } from "./graphEditorProvider";
import type { GraphEditorAction } from "./graphEditorProtocol";
import { knobPointerLockVersionWarning } from "./editorCompatibility";
import { NativeClientHelper } from "./nativeClientHelper";
import { resolveIntravenousBinary } from "./serverBinaryPaths";
import { prepareVSCodeRelaunch, readVSCodeXWaylandRelaunch } from "./vscodeRelaunch";

function configureXWaylandRelaunch(context: vscode.ExtensionContext, localDesktop: boolean, log: (message: string) => void): void {
    const key = "autoRelaunchWithXWayland";
    const configuration = () => vscode.workspace.getConfiguration("intravenous");
    const enabled = () => configuration().inspect<boolean>(key)?.globalValue === true;
    const applicable = localDesktop && process.platform === "linux" && process.env.XDG_SESSION_TYPE === "wayland"
        && process.env.INTRAVENOUS_XWAYLAND_RELAUNCH !== "1";
    let pending: ChildProcess | null = null;
    let attempted = false;
    let preparing = false;
    const cancel = () => {
        pending?.kill();
        pending = null;
    };
    async function relaunchIfEnabled() {
        if (!applicable || attempted || preparing || !enabled()) return;
        attempted = true;
        preparing = true;
        try {
            const plan = readVSCodeXWaylandRelaunch();
            if (!plan) return;
            const worker = await prepareVSCodeRelaunch(context.asAbsolutePath("dist/vscodeRelaunchWorker.js"), plan);
            if (!enabled()) { worker?.kill(); return; }
            pending = worker;
            if (!pending) return;
            attempted = true;
            log("Requesting a normal VS Code quit. The launcher will reopen the editor through XWayland.");
            await vscode.commands.executeCommand("workbench.action.quit");
        } catch (error) {
            cancel();
            log(`Intravenous could not arrange an XWayland restart. ${error instanceof Error ? error.message : String(error)}`);
        } finally {
            preparing = false;
        }
    }
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((event) => {
        if (!event.affectsConfiguration(`intravenous.${key}`)) return;
        if (!enabled()) { attempted = false; cancel(); }
        else void relaunchIfEnabled();
    }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.enableXWaylandRelaunch", async () => {
        await configuration().update(key, true, vscode.ConfigurationTarget.Global);
        await relaunchIfEnabled();
    }));
    void relaunchIfEnabled();
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    const outputChannel = timestampOutput(vscode.window.createOutputChannel("Intravenous"));
    const provider = new LiveGraphViewProvider(context.extensionUri);
    const highlighter = new NodeSpanHighlighter();
    const sessionFactory = container.resolve(WorkspaceSessionFactory);

    context.subscriptions.push(outputChannel);
    context.subscriptions.push(highlighter);
    context.subscriptions.push(vscode.window.registerWebviewViewProvider("intravenous.liveGraph", provider, {
        webviewOptions: {
            retainContextWhenHidden: true,
        },
    }));

    const workspaceFolder = vscode.workspace.workspaceFolders?.[0];
    const localDesktop = vscode.env.uiKind === vscode.UIKind.Desktop
        && (!vscode.env.remoteName || context.extension.extensionKind === vscode.ExtensionKind.UI);
    configureXWaylandRelaunch(context, localDesktop, (message) => outputChannel.appendLine(message));
    let nativeHelper: NativeClientHelper | undefined;
    if (process.platform === "linux" && localDesktop) {
        const name = "intravenous-client-helper";
        const packaged = context.asAbsolutePath(`dist/${name}`);
        nativeHelper = new NativeClientHelper(() => {
            if (existsSync(packaged)) return NativeClientHelper.launch(packaged);
            const configured = vscode.workspace.getConfiguration("intravenous", workspaceFolder?.uri).get<string>("intravenousDir");
            const binary = resolveIntravenousBinary(name, workspaceFolder?.uri.fsPath, configured);
            return NativeClientHelper.launch(binary.path);
        }, (message) => outputChannel.appendLine(message));
        context.subscriptions.push(nativeHelper);
        nativeHelper.start();
    } else if (vscode.workspace.getConfiguration("intravenous").get("warpCursorAfterPointDrag", true)) {
        outputChannel.appendLine("Automation cursor return is unavailable. It requires a local Linux desktop extension host with X11 support.");
    }
    if (!workspaceFolder) {
        return;
    }

    const session = sessionFactory.create(
        workspaceFolder,
        outputChannel,
        provider,
        highlighter,
    );

    const graphEditor = new GraphEditorProvider(context.extensionUri, session, nativeHelper, (message) => outputChannel.appendLine(message));
    provider.setInteractionHintHandler((hint) => graphEditor.setInspectionHint(hint));
    context.subscriptions.push(graphEditor);
    for (const seek of [false, true]) context.subscriptions.push(vscode.commands.registerCommand(
        seek ? "intravenous.seekPlayback" : "intravenous.togglePlayback", async () => {
            try { await graphEditor.playbackAction(seek); }
            catch (error) { void vscode.window.showErrorMessage(`Cannot change playback: ${error.message}`); }
        }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.openGraphEditor", () => graphEditor.open()));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.traceAutomationCursorReturn", () => graphEditor.traceNextAutomationCursorReturn()));
    const graphActions: GraphEditorAction[] = ["addNode", "editNode", "deleteNode", "openNodeSource", "createPackage", "deleteConnection", "undo", "redo"];
    for (const action of graphActions) {
        context.subscriptions.push(vscode.commands.registerCommand(`intravenous.${action}`, async () => {
            try { await graphEditor.runAction(action); }
            catch (error) { void vscode.window.showErrorMessage(`Cannot edit project: ${error.message}`); }
        }));
    }
    context.subscriptions.push(vscode.window.registerWebviewPanelSerializer("intravenous.graphEditor", {
        async deserializeWebviewPanel(panel) { graphEditor.revive(panel); },
    }));

    context.subscriptions.push(vscode.commands.registerCommand("intravenous.saveProject", async () => {
        try {
            await session.saveProject();
        } catch (error: any) {
            outputChannel.appendLine(`Intravenous project save failed: ${error.message}`);
        }
    }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.enableProjectAutosave", async () => {
        try {
            await session.enableProjectAutosave();
            void vscode.window.showInformationMessage("Intravenous project autosave enabled");
        } catch (error: any) {
            outputChannel.appendLine(`Intravenous autosave enable failed: ${error.message}`);
        }
    }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.disableProjectAutosave", async () => {
        try {
            await session.disableProjectAutosave();
            void vscode.window.showInformationMessage("Intravenous project autosave disabled");
        } catch (error: any) {
            outputChannel.appendLine(`Intravenous autosave disable failed: ${error.message}`);
        }
    }));

    provider.setControlHandler(async (message) => session.dispatchLiveGraphControl(message));
    provider.setNodeEventHandler((target, event, id) => session.nodeEvents.send("source-inspection", target, event, id),
        (target, id, begin) => begin ? session.nodeEvents.begin("source-inspection", target, id)
            : session.nodeEvents.end("source-inspection", target, id),
        () => session.nodeEvents.releaseOwner("source-inspection"));

    context.subscriptions.push({ dispose: () => void session.shutdown() });
    context.subscriptions.push(vscode.window.onDidChangeVisibleTextEditors(() => {
        highlighter.refresh();
    }));
    context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor((editor) => {
        if (editor) {
            highlighter.applyToEditor(editor);
            void session.updateFromEditor(editor).then((nodes) => {
                session.updatePrimaryHighlight(nodes);
            }, (error: Error) => {
                outputChannel.appendLine(`Intravenous editor query failed: ${error.message}`);
            });
        }
    }));

    if (!session.isIntravenousProject()) {
        outputChannel.appendLine(`workspace is not an Intravenous project: missing ${session.projectMarkerPath()}`);
        return;
    }

    if (vscode.env.uiKind === vscode.UIKind.Desktop) {
        const warning = knobPointerLockVersionWarning(vscode.env.appName, vscode.version);
        if (warning) {
            outputChannel.appendLine(warning);
            void vscode.window.showWarningMessage(warning);
        }
    }

    context.subscriptions.push(vscode.languages.registerDocumentHighlightProvider(
        { scheme: "file", language: "cpp" },
        {
            async provideDocumentHighlights(document, position) {
                const owningWorkspace = vscode.workspace.getWorkspaceFolder(document.uri);
                if (!owningWorkspace || owningWorkspace.uri.fsPath !== session.workspaceRoot()) {
                    return undefined;
                }

                try {
                    if (!(await session.ensureReady())) {
                        return undefined;
                    }
                } catch {
                    return undefined;
                }

                try {
                    if (await session.hasNodesAtPosition(document, position)) {
                        return [];
                    }
                } catch {
                }

                return undefined;
            },
        },
    ));

    try {
        await session.start();
        if (vscode.window.activeTextEditor) {
            const nodes = await session.updateFromEditor(vscode.window.activeTextEditor);
            session.updatePrimaryHighlight(nodes);
        }
    } catch (error: any) {
        outputChannel.appendLine(`Intravenous startup failed: ${error.message}`);
        throw error;
    }

    context.subscriptions.push(vscode.window.onDidChangeTextEditorSelection(async (event) => {
        if (event.textEditor.document.uri.scheme !== "file") {
            return;
        }
        try {
            const nodes = await session.updateFromEditor(event.textEditor);
            session.updatePrimaryHighlight(nodes);
        } catch (error: any) {
            const message = `Intravenous query failed: ${error.message}`;
            if (message !== session.lastQueryError) {
                outputChannel.appendLine(message);
                session.lastQueryError = message;
            }
        }
    }));
}

export function deactivate(): void {}
