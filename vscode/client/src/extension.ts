import "reflect-metadata";

import { closeSync, existsSync, openSync } from "node:fs";
import { join } from "node:path";
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

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    const bundleMarker = context.asAbsolutePath(".iv-dev-bundle");
    let bundledRuntime: string | undefined;
    if (existsSync(bundleMarker)) {
        // On Linux Nix sees the open store file as a process-lifetime root.
        const bundle = openSync(bundleMarker, "r");
        context.subscriptions.push(new vscode.Disposable(() => closeSync(bundle)));
        bundledRuntime = join(context.extensionPath, "..", "runtime");
    }
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
    let nativeHelper: NativeClientHelper | undefined;
    if (process.platform === "linux" && localDesktop) {
        const name = "intravenous-client-helper";
        nativeHelper = new NativeClientHelper(() => {
            const configured = vscode.workspace.getConfiguration("intravenous", workspaceFolder?.uri).get<string>("intravenousDir");
            const binary = resolveIntravenousBinary(name, configured, bundledRuntime);
            return NativeClientHelper.launch(binary.path);
        }, (message) => outputChannel.appendLine(message));
        context.subscriptions.push(nativeHelper);
        nativeHelper.start();
    } else {
        outputChannel.appendLine("Native cursor return is unavailable. It requires a local Linux desktop extension host.");
    }
    if (!workspaceFolder) {
        return;
    }

    const session = sessionFactory.create(
        workspaceFolder,
        outputChannel,
        provider,
        highlighter,
        bundledRuntime,
    );

    const graphEditor = new GraphEditorProvider(context.extensionUri, session, outputChannel, nativeHelper, context.globalState);
    provider.setInteractionHintHandler((hint) => graphEditor.setInspectionHint(hint));
    context.subscriptions.push(graphEditor);
    for (const seek of [false, true]) context.subscriptions.push(vscode.commands.registerCommand(
        seek ? "intravenous.seekPlayback" : "intravenous.togglePlayback", async () => {
            try { await graphEditor.playbackAction(seek); }
            catch (error) { void vscode.window.showErrorMessage(`Cannot change playback: ${error.message}`); }
        }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.openGraphEditor", () => graphEditor.open()));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.retryServer", async () => {
        try { await session.retryServer(); }
        catch (error) { void vscode.window.showErrorMessage(`Cannot start Intravenous: ${error.message}`); }
    }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.debug", () => graphEditor.debug()));
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
            outputChannel.appendLine(`Project save failed: ${error.message}`);
        }
    }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.enableProjectAutosave", async () => {
        try {
            await session.enableProjectAutosave();
            void vscode.window.showInformationMessage("Project autosave enabled");
        } catch (error: any) {
            outputChannel.appendLine(`Autosave enable failed: ${error.message}`);
        }
    }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.disableProjectAutosave", async () => {
        try {
            await session.disableProjectAutosave();
            void vscode.window.showInformationMessage("Project autosave disabled");
        } catch (error: any) {
            outputChannel.appendLine(`Autosave disable failed: ${error.message}`);
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
            void session.updateFromEditor(editor).catch((error: Error) => {
                outputChannel.appendLine(`Editor query failed: ${error.message}`);
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
            await session.updateFromEditor(vscode.window.activeTextEditor);
        }
    } catch (error: any) {
        outputChannel.appendLine(`Startup failed: ${error.message}`);
    }

    context.subscriptions.push(vscode.window.onDidChangeTextEditorSelection(async (event) => {
        if (event.textEditor !== vscode.window.activeTextEditor || event.textEditor.document.uri.scheme !== "file") {
            return;
        }
        try {
            await session.updateFromEditor(event.textEditor);
        } catch (error: any) {
            const message = `Query failed: ${error.message}`;
            if (message !== session.lastQueryError) {
                outputChannel.appendLine(message);
                session.lastQueryError = message;
            }
        }
    }));
}

export function deactivate(): void {}
