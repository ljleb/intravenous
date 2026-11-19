import "reflect-metadata";

import type { ChildProcess } from "node:child_process";
import { existsSync } from "node:fs";
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
import { prepareVSCodeRelaunch, readVSCodeXWaylandRelaunch } from "./vscodeRelaunch";

async function configureXWaylandRelaunch(context: vscode.ExtensionContext, folder: vscode.WorkspaceFolder | undefined,
    localDesktop: boolean, log: (message: string) => void): Promise<void> {
    if (!folder || !localDesktop || process.platform !== "linux") return;
    try {
        const project = await vscode.workspace.fs.stat(vscode.Uri.joinPath(folder.uri, "iv_project.jsonl"));
        if (!(project.type & vscode.FileType.File)) return;
    } catch {
        return;
    }

    const key = "autoRelaunchWithXWayland";
    const configuration = () => vscode.workspace.getConfiguration("intravenous");
    const preference = () => configuration().get<boolean | null>(key, null);
    const wayland = process.env.XDG_SESSION_TYPE === "wayland"
        || (!process.env.XDG_SESSION_TYPE && !!process.env.WAYLAND_DISPLAY);
    let pending: ChildProcess | null = null;
    let attempted = false;
    let preparing = false;
    const cancel = () => {
        const worker = pending;
        pending = null;
        worker?.kill();
    };
    type Trigger = "activation" | "settings change" | "notification";
    function restartPlan(trigger: Trigger) {
        const sessionType = process.env.XDG_SESSION_TYPE || "unset";
        log(`Checking XWayland restart on ${trigger}. `
            + `XDG_SESSION_TYPE: ${JSON.stringify(sessionType)}. WAYLAND_DISPLAY: ${process.env.WAYLAND_DISPLAY ? "set" : "unset"}.`);
        const plan = readVSCodeXWaylandRelaunch(undefined, log, wayland);
        if (plan && process.env.INTRAVENOUS_XWAYLAND_RELAUNCH === "1") {
            log("Automatic XWayland restart stopped. The relaunched desktop process has no --ozone-platform flag.");
            return null;
        }
        return plan;
    }
    async function relaunchIfEnabled(trigger: Trigger) {
        if (preference() !== true || preparing || pending || attempted) return;
        attempted = true;
        preparing = true;
        try {
            const plan = restartPlan(trigger);
            if (!plan) return;
            const logFile = join(context.logUri.fsPath, "xwayland-restart.log");
            const worker = await prepareVSCodeRelaunch(context.asAbsolutePath("dist/vscodeRelaunchWorker.js"), plan, { logFile, report: log });
            if (preference() !== true) {
                worker?.kill();
                log("XWayland restart cancelled because the setting was turned off.");
                return;
            }
            if (!worker) {
                log(`An XWayland restart launcher from another window is already waiting for editor PID ${plan.pid}.`);
                return;
            }
            pending = worker;
            worker.once("exit", (code, signal) => {
                if (pending !== worker) return;
                pending = null;
                log(`XWayland restart launcher exited while the extension host was still running (code ${code}, signal ${signal}).`);
            });
            log(`XWayland restart launcher ${worker.pid} is ready. Requesting a normal VS Code quit for editor PID ${plan.pid}.`);
            await vscode.commands.executeCommand("workbench.action.quit");
            log("The VS Code quit command returned. The launcher is waiting for the editor process to exit.");
        } catch (error) {
            cancel();
            log(`Could not arrange an XWayland restart. ${error instanceof Error ? error.message : String(error)}`);
        } finally {
            preparing = false;
        }
    }
    context.subscriptions.push(vscode.workspace.onDidChangeConfiguration((event) => {
        if (!event.affectsConfiguration(`intravenous.${key}`)) return;
        if (preference() !== true) {
            if (pending) log("XWayland restart cancelled because the setting was turned off.");
            attempted = false;
            cancel();
        } else void relaunchIfEnabled("settings change");
    }));

    if (preference() === true) {
        await relaunchIfEnabled("activation");
        return;
    }
    if (preference() === false || !wayland) return;
    try {
        if (!restartPlan("activation")) return;
        const enable = "Enable Auto-Restart";
        const decline = "Keep Disabled";
        const selected = await vscode.window.showWarningMessage(
            "Restart VS Code with XWayland to improve cursor wrapping and cursor return after dragging. "
            + "This is optional. Enabling automatic restart restarts all open VS Code windows now.",
            enable, decline);
        // A closed notification leaves the decision unset. A setting changed
        // while it was visible takes precedence over the old notification.
        if (selected === undefined || preference() !== null) return;
        await configuration().update(key, selected === enable, vscode.ConfigurationTarget.Global);
        if (selected === enable) await relaunchIfEnabled("notification");
    } catch (error) {
        log(`Could not offer automatic XWayland restart. ${error instanceof Error ? error.message : String(error)}`);
    }
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
    void configureXWaylandRelaunch(context, workspaceFolder, localDesktop, (message) => outputChannel.appendLine(message));
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
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.profileObservations", () => graphEditor.profileObservations()));
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
            void session.updateFromEditor(editor).then((nodes) => {
                session.updatePrimaryHighlight(nodes);
            }, (error: Error) => {
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
            const nodes = await session.updateFromEditor(vscode.window.activeTextEditor);
            session.updatePrimaryHighlight(nodes);
        }
    } catch (error: any) {
        outputChannel.appendLine(`Startup failed: ${error.message}`);
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
            const message = `Query failed: ${error.message}`;
            if (message !== session.lastQueryError) {
                outputChannel.appendLine(message);
                session.lastQueryError = message;
            }
        }
    }));
}

export function deactivate(): void {}
