import { injectable } from "tsyringe";

import { LiveGraphControlHandler } from "./liveGraphProtocol";
import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { ModulesViewProvider } from "./modulesViewProvider";
import { WorkspaceSession } from "./workspaceSession";

type LiveGraphProviderLike = {
    setControlHandler(handler: LiveGraphControlHandler): void;
    setInstances(instances: unknown[]): void;
    setNodes(nodes: unknown[]): void;
    upsertNodes(nodes: unknown[], replaceInstanceIds?: string[]): void;
    setSelectedInstanceId(instanceId: string | null): void;
    setModuleSource(moduleRoot: string | null): void;
};

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
