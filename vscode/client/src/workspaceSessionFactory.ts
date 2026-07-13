import { injectable } from "tsyringe";
import * as vscode from "vscode";

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

type LaneViewProviderLike = {
    isOpen(): boolean;
    open(): void;
    revive(panel: vscode.WebviewPanel, state: unknown): void;
    setCloseHandler(handler: () => void): void;
    setViewportHandler(handler: () => void): void;
    setScrubHandler(handler: (sampleIndex: number) => void): void;
    setLaneUiStateHandler(handler: (laneId: string, serializedState: string, expectedRevision?: number) => void): void;
    clear(): void;
    setLanes(result: Record<string, unknown>, preserveViewport?: boolean): void;
    setModuleInstances(instances: unknown[]): void;
    viewportState(): { startIndex: number; visibleLaneCount: number };
    setLaneViewId(viewId: string): void;
};

type ModulesViewProviderLike = Pick<ModulesViewProvider, "setState">;

@injectable()
export class WorkspaceSessionFactory {
    create(
        workspaceFolder: vscode.WorkspaceFolder,
        outputChannel: vscode.OutputChannel,
        provider: LiveGraphProviderLike,
        laneProvider: LaneViewProviderLike,
        modulesProvider: ModulesViewProviderLike,
        highlighter: NodeSpanHighlighter,
    ): WorkspaceSession {
        return new WorkspaceSession(
            workspaceFolder,
            outputChannel,
            provider,
            laneProvider,
            modulesProvider,
            highlighter,
        );
    }
}
