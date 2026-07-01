import { injectable } from "tsyringe";
import * as vscode from "vscode";

import { LiveGraphControlHandler } from "./liveGraphProtocol";
import { NodeSpanHighlighter } from "./nodeSpanHighlighter";
import { WorkspaceSession } from "./workspaceSession";

type LiveGraphProviderLike = {
    clearSampleInputValueOverride(nodeId: string, memberOrdinal: number, inputOrdinal: number): void;
    setControlHandler(handler: LiveGraphControlHandler): void;
    setNodes(nodes: unknown[]): void;
    updateSampleInputValue(nodeId: string, inputOrdinal: number, value: unknown, memberOrdinal?: number | null): void;
    pruneDeletedNodeState(nodeIds: string[]): void;
};

type LaneViewProviderLike = {
    isOpen(): boolean;
    open(): void;
    revive(panel: vscode.WebviewPanel, state: unknown): void;
    setCloseHandler(handler: () => void): void;
    setViewportHandler(handler: () => void): void;
    clear(): void;
    setLanes(result: Record<string, unknown>): void;
    viewportState(): { startIndex: number; visibleLaneCount: number };
};

@injectable()
export class WorkspaceSessionFactory {
    create(
        workspaceFolder: vscode.WorkspaceFolder,
        outputChannel: vscode.OutputChannel,
        provider: LiveGraphProviderLike,
        laneProvider: LaneViewProviderLike,
        highlighter: NodeSpanHighlighter,
    ): WorkspaceSession {
        return new WorkspaceSession(
            workspaceFolder,
            outputChannel,
            provider,
            laneProvider,
            highlighter,
        );
    }
}
