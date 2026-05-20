const vscode = require("vscode");

const { LiveGraphViewProvider } = require("./liveGraphViewProvider");
const { LaneViewProvider } = require("./lanesViewProvider");
const { NodeSpanHighlighter } = require("./nodeSpanHighlighter");
const { WorkspaceSession } = require("./workspaceSession");

async function activate(context) {
    const outputChannel = vscode.window.createOutputChannel("Intravenous");
    const provider = new LiveGraphViewProvider(context.extensionUri);
    const laneProvider = new LaneViewProvider();
    const highlighter = new NodeSpanHighlighter();
    context.subscriptions.push(outputChannel);
    context.subscriptions.push(highlighter);
    context.subscriptions.push(vscode.window.registerWebviewViewProvider("intravenous.liveGraph", provider, {
        webviewOptions: {
            retainContextWhenHidden: true,
        },
    }));

    const workspaceFolder = vscode.workspace.workspaceFolders && vscode.workspace.workspaceFolders[0];
    if (!workspaceFolder) {
        return;
    }

    const session = new WorkspaceSession(workspaceFolder, outputChannel, provider, laneProvider, highlighter);
    laneProvider.setCloseHandler(() => {
        session.closeLaneView().catch((error) => {
            outputChannel.appendLine(`Intravenous lane view close failed: ${error.message}`);
        });
    });
    laneProvider.setViewportHandler(() => {
        session.updateLaneViewVisibleLanes().catch((error) => {
            outputChannel.appendLine(`Intravenous lane viewport update failed: ${error.message}`);
        });
    });
    context.subscriptions.push(vscode.window.registerWebviewPanelSerializer("intravenous.lanes", {
        async deserializeWebviewPanel(panel, state) {
            laneProvider.revive(panel, state);
            if (session.client) {
                try {
                    await session.openLaneView();
                } catch (error) {
                    outputChannel.appendLine(`Intravenous lane view restore failed: ${error.message}`);
                }
            }
        },
    }));
    context.subscriptions.push(vscode.commands.registerCommand("intravenous.openLanes", async () => {
        laneProvider.open();
        try {
            await session.openLaneView();
        } catch (error) {
            outputChannel.appendLine(`Intravenous lane query failed: ${error.message}`);
        }
    }));
    provider.setControlHandler(async (message) => {
        const memberOrdinal = message.memberOrdinal == null ? null : Number(message.memberOrdinal);
        if (message.type === "clearSampleInputValueOverride") {
            if (memberOrdinal == null) {
                return;
            }
            await session.clearSampleInputValueOverride(message.nodeId, memberOrdinal, Number(message.inputOrdinal));
            return;
        }
        await session.setSampleInputValue(
            message.nodeId,
            Number(message.inputOrdinal),
            message.value,
            memberOrdinal
        );
    });
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
            provideDocumentHighlights: async (document, position) => {
                const owningWorkspace = vscode.workspace.getWorkspaceFolder(document.uri);
                if (!owningWorkspace || owningWorkspace.uri.fsPath !== session.workspaceRoot()) {
                    return undefined;
                }

                try {
                    if (!(await session.ensureReady())) {
                        return undefined;
                    }
                } catch (_) {
                    return undefined;
                }

                try {
                    const result = await session.client.request("graph.queryBySpans", {
                        filePath: document.uri.fsPath,
                        ranges: [{
                            start: { line: position.line + 1, column: position.character + 1 },
                            end: { line: position.line + 1, column: position.character + 1 },
                        }],
                        match: "intersection",
                    });

                    if (Array.isArray(result.nodes) && result.nodes.length > 0) {
                        return [];
                    }
                } catch (_) {
                }

                return undefined;
            },
        }
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
    } catch (error) {
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
        } catch (error) {
            const message = `Intravenous query failed: ${error.message}`;
            if (message !== session.lastQueryError) {
                outputChannel.appendLine(message);
                session.lastQueryError = message;
            }
        }
    }));
}

function deactivate() {}

module.exports = {
    activate,
    deactivate,
};
