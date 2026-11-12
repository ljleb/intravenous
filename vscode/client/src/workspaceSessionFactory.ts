import type * as vscode from "vscode";
import { injectable } from "tsyringe";

import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { ModulesViewProvider } from "./modulesViewProvider";
import { WorkspaceSession } from "./workspaceSession";

type LiveGraphProviderLike = WorkspaceSession["provider"];

type ModulesViewProviderLike = Pick<ModulesViewProvider, "setState">;

@injectable()
export class WorkspaceSessionFactory {
    create(
        workspaceFolder: vscode.WorkspaceFolder,
        outputChannel: vscode.OutputChannel,
        provider: LiveGraphProviderLike,
        modulesProvider: ModulesViewProviderLike,
        highlighter: NodeSpanHighlighter,
    ): WorkspaceSession {
        return new WorkspaceSession(
            workspaceFolder,
            outputChannel,
            provider,
            modulesProvider,
            highlighter,
        );
    }
}
