import "reflect-metadata";

import * as vscode from "vscode";
import { container } from "tsyringe";

import { LiveGraphViewProvider } from "./liveGraphViewProvider";
import { LaneViewProvider } from "./lanesViewProvider";
import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { WorkspaceSessionFactory } from "./workspaceSessionFactory";

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    const outputChannel = vscode.window.createOutputChannel("Intravenous");
    const provider = new LiveGraphViewProvider(context.extensionUri);
    const laneProvider = new LaneViewProvider();
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
    if (!workspaceFolder) {
        return;
    }

    const session = sessionFactory.create(workspaceFolder, outputChannel, provider, laneProvider, highlighter);
    laneProvider.setCloseHandler(() => {
        session.closeLaneView().catch((error: Error) => {
            outputChannel.appendLine(`Intravenous lane view close failed: ${error.message}`);
        });
    });
    laneProvider.setViewportHandler(() => {
        session.updateLaneViewVisibleLanes().catch((error: Error) => {
            outputChannel.appendLine(`Intravenous lane viewport update failed: ${error.message}`);
        });
    });

    context.subscriptions.push(vscode.window.registerWebviewPanelSerializer("intravenous.lanes", {
        async deserializeWebviewPanel(panel, state) {
            laneProvider.revive(panel, state);
            try {
                await session.openLaneView();
            } catch (error: any) {
                outputChannel.appendLine(`Intravenous lane view restore failed: ${error.message}`);
            }
        },
    }));

    context.subscriptions.push(vscode.commands.registerCommand("intravenous.openLanes", async () => {
        laneProvider.open();
        try {
            await session.openLaneView();
        } catch (error: any) {
            outputChannel.appendLine(`Intravenous lane query failed: ${error.message}`);
        }
    }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.resumePlayback", async () => {
        try {
            await session.resumePlayback(0);
        } catch (error: any) {
            outputChannel.appendLine(`Intravenous resume failed: ${error.message}`);
        }
    }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.pausePlayback", async () => {
        try {
            await session.pausePlayback();
        } catch (error: any) {
            outputChannel.appendLine(`Intravenous pause failed: ${error.message}`);
        }
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

    context.subscriptions.push({ dispose: () => void session.shutdown() });
    context.subscriptions.push(vscode.window.onDidChangeVisibleTextEditors(() => {
        highlighter.refresh();
    }));
    context.subscriptions.push(vscode.window.onDidChangeActiveTextEditor((editor) => {
        if (editor) {
            highlighter.applyToEditor(editor);
        }
    }));

    if (!session.isIntravenousProject()) {
        outputChannel.appendLine(`workspace is not an Intravenous project: missing ${session.projectMarkerPath()}`);
        return;
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
        if (laneProvider.isOpen()) {
            await session.openLaneView();
        }
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
