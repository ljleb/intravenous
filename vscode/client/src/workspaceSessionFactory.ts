import type * as vscode from "vscode";
import { injectable } from "tsyringe";

import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { WorkspaceSession } from "./workspaceSession";

type LiveGraphProviderLike = WorkspaceSession["provider"];


@injectable()
export class WorkspaceSessionFactory {
    create(
        workspaceFolder: vscode.WorkspaceFolder,
        outputChannel: vscode.OutputChannel,
        provider: LiveGraphProviderLike,
        highlighter: NodeSpanHighlighter,
    ): WorkspaceSession {
        return new WorkspaceSession(
            workspaceFolder,
            outputChannel,
            provider,
            highlighter,
        );
    }
}
