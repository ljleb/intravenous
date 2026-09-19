import "reflect-metadata";

import * as vscode from "vscode";
import { container } from "tsyringe";

import { LiveGraphViewProvider } from "./liveGraphViewProvider";
import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { WorkspaceSessionFactory } from "./workspaceSessionFactory";
import { ModulesViewProvider } from "./modulesViewProvider";
import { timestampOutput } from "./outputLog";


export async function activate(context: vscode.ExtensionContext): Promise<void> {
    const outputChannel = timestampOutput(vscode.window.createOutputChannel("Intravenous"));
    const provider = new LiveGraphViewProvider(context.extensionUri);
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
        outputChannel,
        provider,
        modulesProvider,
        highlighter,
    );
    modulesProvider.setControlHandler((message) => session.dispatchModulesControl(message));

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

    context.subscriptions.push(vscode.commands.registerCommand("intravenous.openModules", async () => {
        modulesProvider.open();
        try {
            await session.refreshModulesPanel();
        } catch (error: any) {
            outputChannel.appendLine(`Intravenous modules refresh failed: ${error.message}`);
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
