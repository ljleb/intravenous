import "reflect-metadata";

import * as vscode from "vscode";
import { container } from "tsyringe";

import { LiveGraphViewProvider } from "./liveGraphViewProvider";
import { LaneViewProvider } from "./lanesViewProvider";
import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { WorkspaceSessionFactory } from "./workspaceSessionFactory";
import { ModulesViewProvider } from "./modulesViewProvider";

let deactivating = false;

export async function activate(context: vscode.ExtensionContext): Promise<void> {
    deactivating = false;
    const outputChannel = vscode.window.createOutputChannel("Intravenous");
    const provider = new LiveGraphViewProvider(context.extensionUri);
    const laneProvider = new LaneViewProvider();
    const modulesProvider = new ModulesViewProvider();
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

    const session = sessionFactory.create(
        workspaceFolder,
        outputChannel,
        provider,
        laneProvider,
        modulesProvider,
        highlighter,
    );
    laneProvider.setCloseHandler(() => {
        // Panel disposal while VS Code/the extension is shutting down is not
        // an authored deletion.  An ordinary user panel close is.
        if (deactivating) return;
        session.closeLaneView().catch((error: Error) => {
            outputChannel.appendLine(`Intravenous lane view close failed: ${error.message}`);
        });
    });
    laneProvider.setViewportHandler(() => {
        session.updateLaneViewVisibleLanes().catch((error: Error) => {
            outputChannel.appendLine(`Intravenous lane viewport update failed: ${error.message}`);
        });
    });
    laneProvider.setScrubHandler((sampleIndex) => {
        session.seekPlayback(sampleIndex).catch((error: Error) => {
            outputChannel.appendLine(`Intravenous seek playback failed: ${error.message}`);
        });
    });
    laneProvider.setLaneUiStateHandler((laneId, serializedState, expectedRevision) => {
        session.setTimelineLaneUiState(laneId, serializedState, expectedRevision).catch((error: Error) => {
            outputChannel.appendLine(`Intravenous lane UI state update failed: ${error.message}`);
        });
    });
    laneProvider.setDebugHandler((message) => {
        const field = typeof message.field === "string" ? ` ${message.field}` : "";
        const detail = typeof message.detail === "string" ? ` ${message.detail}` : "";
        outputChannel.appendLine(`Intravenous beat pointer${field}: ${String(message.phase || "event")}${detail}`);
    });
    modulesProvider.setControlHandler((message) => session.dispatchModulesControl(message));

    context.subscriptions.push(vscode.window.registerWebviewPanelSerializer("intravenous.lanes", {
        async deserializeWebviewPanel(panel, state) {
            laneProvider.revive(panel, state);
            session.restoreLaneViewId(laneProvider.currentLaneViewId());
            try {
                await session.openLaneView();
            } catch (error: any) {
                outputChannel.appendLine(`Intravenous lane view restore failed: ${error.message}`);
            }
        },
    }));
    context.subscriptions.push(vscode.window.registerWebviewPanelSerializer("intravenous.modules", {
        async deserializeWebviewPanel(panel) {
            modulesProvider.revive(panel);
            try {
                await session.refreshModulesPanel();
            } catch (error: any) {
                outputChannel.appendLine(`Intravenous modules restore failed: ${error.message}`);
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
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.createLane", async () => {
        let creatableLanes: Array<{ typeId: string; category: string; label: string; description: string }>;
        try {
            creatableLanes = await session.getTimelineLaneTypes();
            outputChannel.appendLine("Intravenous create-lane types: "
                + creatableLanes.map((lane) => lane.typeId).join(", "));
        } catch (error: any) {
            outputChannel.appendLine(`Intravenous lane type query failed: ${error.message}`);
            return;
        }
        const items: vscode.QuickPickItem[] = [];
        let category = "";
        for (const lane of creatableLanes) {
            if (lane.category !== category) {
                category = lane.category;
                items.push({label: category, kind: vscode.QuickPickItemKind.Separator});
            }
            items.push({
                label: lane.label,
                description: lane.description,
                detail: lane.typeId,
            });
        }
        const selected = await vscode.window.showQuickPick(items, {
            title: "Create Timeline Lane",
            placeHolder: "Choose a lane type",
            matchOnDescription: true,
            matchOnDetail: true,
        });
        if (!selected || selected.kind === vscode.QuickPickItemKind.Separator) return;
        const lane = creatableLanes.find((candidate) => candidate.label === selected.label);
        if (!lane) return;
        try {
            await session.createTimelineLane(lane.typeId);
            laneProvider.open();
            await session.openLaneView();
        } catch (error: any) {
            outputChannel.appendLine(`Intravenous lane creation failed: ${error.message}`);
        }
    }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.openModules", async () => {
        modulesProvider.open();
        try {
            await session.refreshModulesPanel();
        } catch (error: any) {
            outputChannel.appendLine(`Intravenous modules refresh failed: ${error.message}`);
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
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.togglePlayback", async () => {
        try {
            await session.togglePlayback();
        } catch (error) {
            outputChannel.appendLine(`Intravenous playback toggle failed: ${error.message}`);
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

export function deactivate(): void {
    deactivating = true;
}
