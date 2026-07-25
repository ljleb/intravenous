// @ts-nocheck
import * as vscode from "vscode";
import { lanePresentationPlugins } from "./lanePlugins";

function utf8ByteOffsetForUtf16Offset(source, utf16Offset) {
    const bounded = Math.max(0, Math.min(source.length, utf16Offset));
    return Buffer.byteLength(source.slice(0, bounded), "utf8");
}

export class LaneViewProvider {
    static instances = new Set();
    static sharedSelection = { laneIds: [], primaryLaneId: "", anchorLaneId: "", anchorViewId: "" };

    constructor() {
        LaneViewProvider.instances.add(this);
        this.panel = null;
        this.lanes = [];
        this.connections = [];
        this.contentByLaneId = {};
        this.uiStateByLaneId = {};
        this.playbackSampleIndex = null;
        this.closeHandler = null;
        this.viewportHandler = null;
        this.scrubHandler = null;
        this.laneUiStateHandler = null;
        this.laneRenameHandler = null;
        this.laneDeleteHandler = null;
        this.captureFilePickerHandler = null;
        this.connectHandler = null;
        this.disconnectHandler = null;
        this.rewireHandler = null;
        this.connectionDebugHandler = null;
        this.debugHandler = null;
        this.startIndex = 0;
        this.visibleLaneCount = 24;
        this.firstSampleIndex = 0;
        this.lastSampleIndex = 0;
        this.displaySampleCount = 0;
        this.totalLaneCount = 0;
        this.instanceNamesByNumericId = new Map();
        this.laneViewId = "";
        this.laneQuery = "";
        this.laneQuerySchema = null;
        this.laneQueryCompletionHandler = null;
    }

    setModuleInstances(instances) {
        this.instanceNamesByNumericId = new Map();
        if (!Array.isArray(instances)) {
            this.postState();
            return;
        }

        for (const instance of instances) {
            const instanceId = typeof instance?.instanceId === "string" ? instance.instanceId : "";
            const displayName = typeof instance?.displayName === "string" && instance.displayName.length > 0
                ? instance.displayName
                : typeof instance?.moduleId === "string" && instance.moduleId.length > 0
                    ? instance.moduleId
                    : typeof instance?.definitionId === "string" ? instance.definitionId : "";
            if (!instanceId || !displayName) {
                continue;
            }
            this.instanceNamesByNumericId.set(moduleInstanceNumericId(instanceId), displayName);
        }
        this.postState();
    }

    setLanes(result, preserveViewport = false) {
        if (!preserveViewport) {
            this.startIndex = Number(result?.startIndex || 0);
            this.visibleLaneCount = Number(result?.visibleLaneCount ?? this.visibleLaneCount ?? 24);
        }
        this.totalLaneCount = Number(result?.totalLaneCount || 0);
        this.lanes = Array.isArray(result?.lanes) ? result.lanes : [];
        this.connections = Array.isArray(result?.connections) ? result.connections : [];
        this.postState();
    }

    clear() {
        this.startIndex = 0;
        this.totalLaneCount = 0;
        this.lanes = [];
        this.connections = [];
        this.contentByLaneId = {};
        this.uiStateByLaneId = {};
        this.playbackSampleIndex = null;
        this.postState();
    }

    setLaneContent(update) {
        if (!update || ( !Array.isArray(update.lanes) && !Array.isArray(update.uiStates))) {
            return;
        }
        for (const content of (Array.isArray(update.lanes) ? update.lanes : [])) {
            const laneId = String(content?.laneId || "");
            if (!laneId) continue;
            this.contentByLaneId[laneId] = {
                adapterType: String(content?.adapterType || ""),
                peakLevel: typeof content?.peakLevel === "number" ? content.peakLevel : null,
                secondaryPeakLevel: typeof content?.secondaryPeakLevel === "number" ? content.secondaryPeakLevel : null,
                sampleChannelType: typeof content?.sampleChannelType === "string" ? content.sampleChannelType : "",
                samples: Array.isArray(content?.samples) ? content.samples.filter((value) => typeof value === "number") : [],
                secondarySamples: Array.isArray(content?.secondarySamples)
                    ? content.secondarySamples.filter((value) => typeof value === "number") : [],
                sampleWindowFirstIndex: Number.isFinite(Number(content?.sampleWindowFirstIndex))
                    ? Number(content.sampleWindowFirstIndex) : null,
                sampleWindowLastIndex: Number.isFinite(Number(content?.sampleWindowLastIndex))
                    ? Number(content.sampleWindowLastIndex) : null,
                eventCount: typeof content?.eventCount === "number"
                    ? content.eventCount
                    : Array.isArray(content?.events) ? content.events.length : 0,
                events: Array.isArray(content?.events) ? content.events : [],
            };
        }
        for (const state of (Array.isArray(update.uiStates) ? update.uiStates : [])) {
            const laneId = String(state?.laneId || "");
            if (!laneId || typeof state?.serializedState !== "string") continue;
            this.uiStateByLaneId[laneId] = {
                revision: typeof state?.revision === "number" ? state.revision : 0,
                serializedState: state.serializedState,
            };
        }
        if (typeof update.playbackSampleIndex === "number") {
            this.playbackSampleIndex = update.playbackSampleIndex;
        }
        this.panel?.webview.postMessage({
            type: "setContent",
            contentByLaneId: this.contentByLaneId,
            uiStateByLaneId: this.uiStateByLaneId,
            playbackSampleIndex: this.playbackSampleIndex,
        });
    }

    isOpen() {
        return this.panel != null;
    }

    setCloseHandler(handler) {
        this.closeHandler = handler;
    }

    setViewportHandler(handler) {
        this.viewportHandler = handler;
    }

    setScrubHandler(handler) { this.scrubHandler = handler; }
    setLaneUiStateHandler(handler) { this.laneUiStateHandler = handler; }
    setLaneRenameHandler(handler) { this.laneRenameHandler = handler; }
    setLaneDeleteHandler(handler) { this.laneDeleteHandler = handler; }
    setLaneDuplicateHandler(handler) { this.laneDuplicateHandler = handler; }
    setCaptureFilePickerHandler(handler) { this.captureFilePickerHandler = handler; }
    setConnectHandler(handler) { this.connectHandler = handler; }
    setDisconnectHandler(handler) { this.disconnectHandler = handler; }
    setRewireHandler(handler) { this.rewireHandler = handler; }
    setConnectionDebugHandler(handler) { this.connectionDebugHandler = handler; }
    setDebugHandler(handler) { this.debugHandler = handler; }

    restoreViewportState(state) {
        if (!state || typeof state !== "object") {
            return;
        }
        this.startIndex = Math.max(0, Number(state.startIndex || 0));
        this.visibleLaneCount = Math.max(1, Number(state.visibleLaneCount || this.visibleLaneCount || 24));
        if (typeof state.laneViewId === "string") this.laneViewId = state.laneViewId;
        if (typeof state.laneQuery === "string") this.laneQuery = state.laneQuery;
    }

    currentLaneViewId() { return this.laneViewId || null; }

    setLaneViewId(viewId) {
        this.laneViewId = viewId;
        this.postState();
    }

    setLaneQuerySchema(schema) {
        this.laneQuerySchema = schema && typeof schema === "object" ? schema : null;
        this.postState();
    }

    setLaneQueryCompletionHandler(handler) {
        this.laneQueryCompletionHandler = handler;
    }

    viewportState() {
        return {
            // Ordering is local to this VS Code lane view.  Keep the filtered
            // result in the view so it can apply its own order before local
            // viewport virtualization; filtering itself remains server-side.
            startIndex: 0,
            visibleLaneCount: Math.max(this.visibleLaneCount, this.totalLaneCount),
            firstSampleIndex: this.firstSampleIndex,
            lastSampleIndex: this.lastSampleIndex,
            displaySampleCount: this.displaySampleCount,
            laneQuery: this.laneQuery,
        };
    }

    serializeLanes() {
        return this.lanes.map((lane) => {
            const metadata = lane && typeof lane.metadata === "object" && lane.metadata ? lane.metadata : {};
            const has = (key) => Object.prototype.hasOwnProperty.call(metadata, key);
            const isAudioOutput = has("audio_output");
            const isAudioInput = has("audio_input");
            const type = has("dsp_graph.event") ? "event" : has("dsp_graph.sample") ? "sample" : "lane";
            const direction = has("dsp_graph.graph_output") ? "out" : has("dsp_graph.graph_input") ? "in" : "";
            const instanceName = has("dsp_graph.public")
                && type === "sample"
                && direction === "out"
                ? this.instanceNamesByNumericId.get(Number(metadata["dsp_graph.module_instance_id"]))
                : "";
            const defaultTitle = isAudioOutput ? "Audio device output"
                : isAudioInput ? "Audio device input"
                : (has("dsp_graph.public") ? "public " : "") + type + (direction ? " " + direction : "")
                    + (instanceName ? ` • ${instanceName}` : "");
            const title = typeof metadata["lane.name"] === "string" && metadata["lane.name"].length > 0
                ? metadata["lane.name"] : defaultTitle;
            const numericMetadata = Object.entries(metadata)
                .filter(([key, value]) => typeof value === "number" && !key.includes("ordinal"))
                .map(([key, value]) => `${key.replace(/^dsp_graph\./, "")}=${value}`);
            return {
                laneId: String(lane.laneId || ""),
                domain: lane.domain || "realtime",
                title,
                description: numericMetadata.join(" • "),
                metadata,
                modelTypeId: typeof lane.modelTypeId === "string" ? lane.modelTypeId : "",
                sampleChannelType: typeof lane.sampleChannelType === "string" ? lane.sampleChannelType : "",
                outputKind: lane.outputKind === "event" ? "event" : "sample",
                inputs: Array.isArray(lane.inputs) ? lane.inputs
                    .filter((input) => input && typeof input === "object")
                    .map((input) => ({
                        domain: input.domain === "compiled" ? "compiled" : "realtime",
                        kind: input.kind === "event" ? "event" : "sample",
                        ordinal: Math.max(0, Math.floor(Number(input.ordinal || 0))),
                        name: typeof input.name === "string" ? input.name : "",
                    })) : [],
            };
        });
    }

    serializeConnections() {
        const lanesById = new Map(this.serializeLanes().map((lane) => [lane.laneId, lane]));
        return this.connections.map((connection) => {
            const sourceLaneId = String(connection.sourceLaneId || "");
            const targetLaneId = String(connection.targetLaneId || "");
            const source = lanesById.get(sourceLaneId);
            const target = lanesById.get(targetLaneId);
            const targetLabel = target
                ? `${target.title} • ${target.description}`
                : `#${targetLaneId}`;
            const sourceLabel = source
                ? `${source.title} • ${source.description}`
                : `#${sourceLaneId}`;
            return {
                sourceLaneId,
                targetLaneId,
                kind: `${connection.portKind || "port"} input ${Number(connection.portOrdinal || 0)}`,
                state: "active",
                portKind: connection.portKind || "",
                portDomain: connection.portDomain === "compiled" ? "compiled" : "realtime",
                portOrdinal: Number(connection.portOrdinal || 0),
                sourceLabel,
                targetLabel,
            };
        });
    }

    open() {
        if (this.panel) {
            this.panel.reveal(vscode.ViewColumn.Beside);
            this.postState();
            return;
        }
        const panel = vscode.window.createWebviewPanel(
            "intravenous.lanes",
            "Intravenous Lanes",
            vscode.ViewColumn.Beside,
            {
                enableScripts: true,
                retainContextWhenHidden: true,
            }
        );
        this.attachPanel(panel);
    }

    revive(panel, state) {
        this.restoreViewportState(state);
        this.attachPanel(panel);
    }

    attachPanel(panel) {
        this.panel = panel;
        this.panel.webview.options = {
            enableScripts: true,
        };
        this.panel.onDidDispose(() => {
            this.panel = null;
            LaneViewProvider.instances.delete(this);
            if (this.closeHandler) {
                this.closeHandler();
            }
        });
        this.panel.webview.onDidReceiveMessage((message) => {
            if (!message) {
                return;
            }
            if (message.type === "laneSelectionChanged") {
                const laneIds = Array.isArray(message.laneIds)
                    ? [...new Set(message.laneIds.filter((laneId) => typeof laneId === "string" && laneId))] : [];
                LaneViewProvider.sharedSelection = {
                    laneIds,
                    primaryLaneId: typeof message.primaryLaneId === "string" ? message.primaryLaneId : "",
                    anchorLaneId: typeof message.anchorLaneId === "string" ? message.anchorLaneId : "",
                    anchorViewId: typeof message.anchorViewId === "string" ? message.anchorViewId : "",
                };
                for (const provider of LaneViewProvider.instances) provider.postSelection();
                return;
            }
            if (message.type === "scrubPlayback") {
                this.scrubHandler?.(Math.max(0, Number(message.sampleIndex || 0)));
                return;
            }
            if (message.type === "setLaneUiState") {
                this.laneUiStateHandler?.(String(message.laneId || ""), String(message.serializedState || ""),
                    typeof message.expectedRevision === "number" ? message.expectedRevision : undefined);
                return;
            }
            if (message.type === "renameLane") {
                this.laneRenameHandler?.(String(message.laneId || ""), String(message.name || ""));
                return;
            }
            if (message.type === "deleteLane") {
                const laneId = String(message.laneId || "");
                this.connectionDebugHandler?.(
                    `host received delete laneId=${laneId || "<empty>"} handler=${this.laneDeleteHandler ? "installed" : "missing"}`);
                if (laneId && this.laneDeleteHandler) {
                    this.laneDeleteHandler(laneId);
                }
                return;
            }
            if (message.type === "duplicateLane") {
                const laneId = String(message.laneId || "");
                if (laneId && this.laneDuplicateHandler) this.laneDuplicateHandler(laneId);
                return;
            }
            if (message.type === "chooseCaptureFile") {
                const laneId = String(message.laneId || "");
                if (laneId && this.captureFilePickerHandler) {
                    this.captureFilePickerHandler(
                        laneId,
                        String(message.currentPath || ""),
                        typeof message.expectedRevision === "number" ? message.expectedRevision : undefined);
                }
                return;
            }
            if (message.type === "connectLanes") {
                const sourceLaneId = String(message.sourceLaneId || "");
                const targetLaneId = String(message.targetLaneId || "");
                const portDomain = message.portDomain === "compiled" ? "compiled" : "realtime";
                const portKind = message.portKind === "event" ? "event" : "sample";
                const portOrdinal = Math.max(0, Math.floor(Number(message.portOrdinal || 0)));
                if (sourceLaneId && targetLaneId && sourceLaneId !== targetLaneId) {
                    this.connectionDebugHandler?.(`host received connect ${sourceLaneId} -> ${targetLaneId} ${portDomain}/${portKind}[${portOrdinal}]`);
                    this.connectHandler?.(sourceLaneId, targetLaneId, portDomain, portKind, portOrdinal);
                } else {
                    this.connectionDebugHandler?.("host rejected malformed connect message");
                }
                return;
            }
            if (message.type === "connectionDebug") {
                this.connectionDebugHandler?.(String(message.message || "webview connection event"));
                return;
            }
            if (message.type === "copyLaneDebugInfo") {
                void vscode.env.clipboard.writeText(String(message.text || ""));
                return;
            }
            if (message.type === "disconnectLanes") {
                const sourceLaneId = String(message.sourceLaneId || "");
                const targetLaneId = String(message.targetLaneId || "");
                const portDomain = message.portDomain === "compiled" ? "compiled" : "realtime";
                const portKind = message.portKind === "event" ? "event" : "sample";
                const portOrdinal = Math.max(0, Math.floor(Number(message.portOrdinal || 0)));
                if (sourceLaneId && targetLaneId) {
                    this.disconnectHandler?.(sourceLaneId, targetLaneId, portDomain, portKind, portOrdinal);
                }
                return;
            }
            if (message.type === "rewireLaneConnection") {
                const sourceLaneId = String(message.sourceLaneId || "");
                const oldTargetLaneId = String(message.oldTargetLaneId || "");
                const targetLaneId = String(message.targetLaneId || "");
                const portDomain = message.portDomain === "compiled" ? "compiled" : "realtime";
                const portKind = message.portKind === "event" ? "event" : "sample";
                const portOrdinal = Math.max(0, Math.floor(Number(message.portOrdinal || 0)));
                if (sourceLaneId && oldTargetLaneId && targetLaneId) {
                    this.rewireHandler?.(sourceLaneId, oldTargetLaneId, targetLaneId, portDomain, portKind, portOrdinal);
                }
                return;
            }
            if (message.type === "beatPointerDebug") {
                this.debugHandler?.(message);
                return;
            }
            if (message.type === "timelineViewportChanged") {
                const firstSampleIndex = Math.max(0, Math.floor(Number(message.firstSampleIndex || 0)));
                const lastSampleIndex = Math.max(firstSampleIndex, Math.floor(Number(message.lastSampleIndex || 0)));
                const displaySampleCount = Math.max(0, Math.floor(Number(message.displaySampleCount || 0)));
                if (firstSampleIndex === this.firstSampleIndex
                    && lastSampleIndex === this.lastSampleIndex
                    && displaySampleCount === this.displaySampleCount) return;
                this.firstSampleIndex = firstSampleIndex;
                this.lastSampleIndex = lastSampleIndex;
                this.displaySampleCount = displaySampleCount;
                this.viewportHandler?.(this.viewportState());
                return;
            }
            if (message.type === "laneQueryChanged") {
                const laneQuery = String(message.laneQuery || "");
                if (laneQuery === this.laneQuery) return;
                this.laneQuery = laneQuery;
                // The query is part of this view's server-side filter. It is
                // intentionally not project state, but it must refresh the
                // current view as soon as the compact field is edited.
                this.viewportHandler?.(this.viewportState());
                return;
            }
            if (message.type === "laneQueryCompletionRequested") {
                const source = String(message.source || "");
                const cursorOffset = Math.max(0, Math.floor(Number(message.cursorOffset || 0)));
                const schemaRevision = Math.max(0, Math.floor(Number(message.schemaRevision || 0)));
                const requestId = Math.max(0, Math.floor(Number(message.requestId || 0)));
                if (!this.laneQueryCompletionHandler) return;
                void this.laneQueryCompletionHandler(
                    source,
                    utf8ByteOffsetForUtf16Offset(source, cursorOffset),
                    schemaRevision,
                ).then((completion) => {
                    this.panel?.webview.postMessage({ type: "laneQueryCompletion", requestId, completion });
                }).catch(() => {
                    this.panel?.webview.postMessage({ type: "laneQueryCompletion", requestId, completion: null });
                });
                return;
            }
            if (message.type !== "viewportChanged") return;
            const startIndex = Math.max(0, Number(message.startIndex || 0));
            const visibleLaneCount = Math.max(0, Number(message.visibleLaneCount || 0));
            if (
                startIndex === this.startIndex
                && visibleLaneCount === this.visibleLaneCount
            ) {
                return;
            }
            this.startIndex = startIndex;
            this.visibleLaneCount = visibleLaneCount;
            if (this.viewportHandler) {
                this.viewportHandler(this.viewportState());
            }
        });
        this.panel.webview.html = this.getHtml();
        this.postState();
    }

    postState() {
        if (!this.panel) {
            return;
        }
        this.panel.webview.postMessage({
            type: "setState",
            laneViewId: this.laneViewId,
            laneQuery: this.laneQuery,
            laneQuerySchema: this.laneQuerySchema,
            startIndex: this.startIndex,
            visibleLaneCount: this.visibleLaneCount,
            totalLaneCount: this.totalLaneCount,
            lanes: this.serializeLanes(),
            connections: this.serializeConnections(),
            contentByLaneId: this.contentByLaneId,
            uiStateByLaneId: this.uiStateByLaneId,
            playbackSampleIndex: this.playbackSampleIndex,
            selection: LaneViewProvider.sharedSelection,
        });
    }

    postSelection() {
        this.panel?.webview.postMessage({ type: "setSelection", selection: LaneViewProvider.sharedSelection });
    }

    getHtml() {
        const nonce = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
        const lanePluginCss = lanePresentationPlugins.map((plugin) => plugin.css || "").join("\n");
        const lanePluginRegistrations = lanePresentationPlugins.map((plugin) =>
            `{typeId:${JSON.stringify(plugin.typeId)},render:${plugin.render.toString()}}`,
        ).join(",\n");
        return `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}';">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
        body {
            margin: 0;
            padding: 0;
            --lane-header-width: 96px;
            background: var(--vscode-sideBar-background);
            color: var(--vscode-foreground);
            font-family: var(--vscode-font-family);
            font-size: var(--vscode-font-size);
            user-select: none;
        }

        #root {
            display: flex;
            flex-direction: column;
            height: 100vh;
            overflow: hidden;
        }

        .summary, .empty, .section-title, .connection-row {
            min-height: 24px;
            padding: 0 8px;
            box-sizing: border-box;
            display: flex;
            align-items: center;
            gap: 6px;
            white-space: nowrap;
            overflow: hidden;
        }

        .summary,
        .empty,
        .section-title {
            color: var(--vscode-descriptionForeground);
        }

        .section-title {
            margin: 0;
            border-bottom: 1px solid var(--vscode-sideBarSectionHeader-border, transparent);
            text-transform: uppercase;
            font-size: 0.8em;
            letter-spacing: 0;
        }

        .lane-viewport {
            position: relative;
            flex: 1 1 auto;
            overflow-y: auto;
            overflow-x: hidden;
            min-height: 80px;
        }

        .lane-spacer {
            width: 1px;
        }

        .lane-window {
            position: absolute;
            top: 0;
            left: 0;
            right: 0;
        }

        .timeline-toolbar {
            min-height: 30px;
            padding: 0 8px;
            display: flex;
            align-items: center;
            gap: 8px;
            border-bottom: 1px solid var(--vscode-sideBarSectionHeader-border, transparent);
            color: var(--vscode-descriptionForeground);
        }

        .timeline-toolbar strong { color: var(--vscode-foreground); letter-spacing: .08em; font-size: .78em; }
        .timeline-toolbar .spacer { flex: 1; }
        .lane-query-toggle {
            height: 21px; padding: 0 6px; border: 0; border-radius: 3px;
            color: var(--vscode-foreground); background: var(--vscode-button-secondaryBackground);
            cursor: pointer; font: inherit; font-size: .78em;
        }
        .lane-query-toggle:hover { background: var(--vscode-button-secondaryHoverBackground); }
        .lane-query-region { position: relative; display: flex; gap: 6px; align-items: center; padding: 4px 8px; border-bottom: 1px solid var(--vscode-sideBarSectionHeader-border, transparent); background: var(--vscode-editor-background); }
        .lane-query-region[hidden] { display: none; }
        .lane-query-region label { color: var(--vscode-descriptionForeground); font-size: .78em; }
        .lane-query-input { flex: 1 1 auto; min-width: 0; height: 22px; box-sizing: border-box; color: var(--vscode-input-foreground); background: var(--vscode-input-background); border: 1px solid var(--vscode-input-border, var(--vscode-editorWidget-border)); font: inherit; }
        .lane-query-suggestions { position: absolute; z-index: 20; top: calc(100% - 3px); left: 78px; right: 8px; max-height: 170px; overflow: auto; border: 1px solid var(--vscode-editorWidget-border); background: var(--vscode-editorWidget-background); box-shadow: 0 2px 8px rgba(0,0,0,.3); }
        .lane-query-suggestion { display: flex; width: 100%; gap: 8px; border: 0; padding: 4px 7px; color: var(--vscode-editor-foreground); background: transparent; text-align: left; font: inherit; cursor: pointer; }
        .lane-query-suggestion.active, .lane-query-suggestion:hover { background: var(--vscode-list-activeSelectionBackground); color: var(--vscode-list-activeSelectionForeground); }
        .lane-query-suggestion-type { margin-left: auto; color: var(--vscode-descriptionForeground); font-size: .82em; }
        .zoom-button {
            width: 21px; height: 21px; padding: 0; cursor: pointer;
            color: var(--vscode-foreground); background: var(--vscode-button-secondaryBackground);
            border: 0; border-radius: 3px;
        }
        .zoom-button:hover { background: var(--vscode-button-secondaryHoverBackground); }
        .zoom-label { min-width: 42px; text-align: center; font-variant-numeric: tabular-nums; }

        .timeline-ruler {
            height: 25px;
            position: relative;
            overflow: hidden;
            border-bottom: 1px solid var(--vscode-sideBarSectionHeader-border, transparent);
            background: var(--vscode-editor-background);
        }
        .timeline-tick { position: absolute; top: 0; bottom: 0; border-left: 1px solid var(--vscode-editorWidget-border, rgba(128,128,128,.25)); padding-left: 3px; font-size: .72em; color: var(--vscode-descriptionForeground); }
        .timeline-tick.minor { top: 12px; color: transparent; opacity: .55; }
        ${lanePluginCss}

        .lane-row:hover,
        .connection-row:hover {
            background: var(--vscode-list-hoverBackground);
        }

        .lane-id {
            flex: 0 0 auto;
            min-width: 18px;
            color: var(--vscode-descriptionForeground);
            font-variant-numeric: tabular-nums;
        }

        .lane-row {
            height: 58px;
            position: relative;
            display: flex;
            align-items: stretch;
            border-bottom: 1px solid var(--vscode-editorWidget-border, rgba(128,128,128,.38));
            box-sizing: border-box;
        }
        /* Cover the playhead through the row separator as well as the meter. */
        .lane-row.realtime { z-index: 6; background: var(--vscode-sideBar-background); }
        .lane-row.selected { box-shadow: inset 3px 0 var(--vscode-focusBorder); }
        .lane-connection-button {
            position: absolute; z-index: 9; left: 0; top: 0;
            width: 24px; height: calc(var(--lane-content-height) - 1px); padding: 0; border: 0; border-radius: 0;
            color: var(--vscode-foreground); background: var(--vscode-button-secondaryBackground);
            cursor: pointer; font: 600 14px/15px var(--vscode-font-family);
        }
        .lane-connection-button:hover { background: var(--vscode-button-secondaryHoverBackground); }
        .lane-connection-button:disabled { opacity: .35; cursor: default; }
        .lane-connection-button.selected { background: var(--vscode-button-background); opacity: 1; }
        .lane-debug-copy-button {
            position: absolute; z-index: 9; left: calc(var(--lane-header-width) - 21px); top: calc(var(--lane-content-height) / 2); transform: translateY(-50%);
            width: 15px; height: 15px; padding: 0; border: 0; border-radius: 2px;
            color: var(--vscode-descriptionForeground); background: transparent; cursor: pointer;
            font: 12px/15px var(--vscode-font-family);
        }
        .lane-debug-copy-button:hover { color: var(--vscode-foreground); background: var(--vscode-toolbar-hoverBackground); }
        .lane-input-wheel {
            position: fixed; z-index: 30; width: 0; height: 0; pointer-events: none;
        }
        .lane-input-wheel-option {
            position: absolute; width: 78px; min-height: 22px; padding: 2px 5px;
            transform: translate(-50%, -50%); border: 1px solid var(--vscode-menu-border, var(--vscode-editorWidget-border));
            border-radius: 3px; color: var(--vscode-menu-foreground); background: var(--vscode-menu-background);
            box-shadow: 0 2px 8px rgba(0,0,0,.35); font: inherit; font-size: .82em; cursor: pointer;
            pointer-events: auto; white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
        }
        .lane-input-wheel-option:hover { background: var(--vscode-menu-selectionBackground); color: var(--vscode-menu-selectionForeground); }
        .connection-overlay { position: absolute; z-index: 6; inset: 0; overflow: visible; pointer-events: none; }
        .selected-connection { fill: none; stroke: var(--vscode-charts-blue); stroke-width: 2; pointer-events: none; }
        .incoming-connection { opacity: .28; stroke-width: 1.5; }
        .outgoing-connection { opacity: .92; }
        .lane-label {
            width: var(--lane-header-width);
            flex: 0 0 var(--lane-header-width);
            min-width: 0;
            display: flex;
            align-items: center;
            gap: 6px;
            padding: 0 28px 0 32px;
            box-sizing: border-box;
            background: var(--vscode-sideBar-background);
            border-right: 1px solid var(--vscode-sideBarSectionHeader-border, rgba(128,128,128,.18));
        }
        .lane-row.realtime .lane-label { display: flex; }
        .lane-row > .lane-label, .lane-row > .lane-track, .lane-row > .realtime-face, .lane-row > .beat-track { height: calc(var(--lane-content-height) - 1px); align-self: flex-start; box-sizing: border-box; }

        .lane-meter {
            width: 74px;
            height: 9px;
            flex: 0 0 74px;
            overflow: hidden;
            border: 1px solid var(--vscode-editorWidget-border, var(--vscode-widget-border));
            background: var(--vscode-editorWidget-border, var(--vscode-widget-border));
        }

        .lane-meter-fill {
            height: 100%;
            background: var(--vscode-charts-green);
        }

        .lane-meter.events .lane-meter-fill {
            background: var(--vscode-charts-purple);
        }

        .title {
            flex: 1 1 auto;
            font-weight: 600;
            overflow: hidden;
            text-overflow: ellipsis;
        }

        .description {
            color: var(--vscode-descriptionForeground);
            flex: 1 1 auto;
            overflow: hidden;
            text-overflow: ellipsis;
        }

        .lane-track {
            position: relative;
            min-width: 180px;
            flex: 1 1 auto;
            overflow: hidden;
            background-color: var(--vscode-editor-background);
        }
        .lane-track::before { content: ""; position: absolute; inset: 0; pointer-events: none; opacity: .5; background-image: repeating-linear-gradient(90deg, transparent 0, transparent calc(var(--second-width) - 1px), var(--vscode-editorWidget-border, rgba(128,128,128,.2)) calc(var(--second-width) - 1px), var(--vscode-editorWidget-border, rgba(128,128,128,.2)) var(--second-width)); }
        .lane-track.capture-track::before { display: none; }
        .compiled-waveform { position: absolute; z-index: 1; inset: 1px 0; width: 100%; height: calc(100% - 2px); pointer-events: none; }
        .capture-range-boundary { position: absolute; z-index: 2; top: 1px; bottom: 1px; width: 1px; background: var(--vscode-charts-orange); opacity: .78; pointer-events: none; }
        .capture-range-boundary.end { background: var(--vscode-charts-blue); }
        .compiled-lane-title { position: absolute; z-index: 2; top: 5px; left: 5px; right: 5px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--vscode-foreground); font-size: .78em; font-weight: 600; line-height: 1; opacity: .7; pointer-events: none; text-shadow: 0 1px 1px var(--vscode-editor-background); }
        .lane-settings-toggle { position: absolute; z-index: 9; left: calc(var(--lane-header-width) - 40px); top: calc(var(--lane-content-height) / 2); transform: translateY(-50%); width: 15px; height: 15px; padding: 0; border: 0; border-radius: 2px; color: var(--vscode-descriptionForeground); background: transparent; font: 12px/15px var(--vscode-font-family); cursor: pointer; }
        .lane-settings-toggle:hover, .lane-settings-toggle[aria-expanded="true"] { background: var(--vscode-toolbar-hoverBackground); }
        .lane-row.settings-open { z-index: 20; }
        .lane-settings-panel { position: absolute; z-index: 20; top: var(--lane-content-height); left: var(--lane-header-width); right: 0; height: 32px; display: flex; align-items: center; gap: 6px; padding: 5px 7px; box-sizing: border-box; border: 1px solid var(--vscode-editorWidget-border, rgba(128,128,128,.35)); background: var(--vscode-editorWidget-background); box-shadow: 0 3px 8px rgba(0,0,0,.28); }
        .playhead { position: absolute; z-index: 3; top: 0; bottom: 0; width: 2px; background: var(--vscode-editorCursor-foreground, var(--vscode-charts-orange)); box-shadow: 0 0 5px var(--vscode-editorCursor-foreground, var(--vscode-charts-orange)); pointer-events: none; }
        .canvas-playhead { position: absolute; z-index: 5; top: 0; bottom: 0; width: 2px; background: var(--vscode-editorCursor-foreground, var(--vscode-charts-orange)); box-shadow: 0 0 5px var(--vscode-editorCursor-foreground, var(--vscode-charts-orange)); pointer-events: none; }
        .realtime-face {
            /* The canvas playhead describes compiled timeline samples, so
               realtime meter faces deliberately cover it. */
            flex: 1 1 auto; min-width: 0; position: relative; display: flex; flex-direction: column; gap: 3px; padding: 5px 3px 3px;
            background: var(--vscode-sideBar-background);
        }
        .realtime-meter-name { min-width: 0; color: var(--vscode-foreground); font-size: .78em; font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .realtime-lane-parameter { min-width: 0; color: var(--vscode-descriptionForeground); font-size: .76em; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .capture-header { min-width: 0; display: flex; flex-direction: column; gap: 0; }
        /* Keep capture titles visually identical to every other realtime lane.
           The compact picker row, not the title, yields the required space. */
        .capture-kind { min-width: 0; height: 15px; line-height: 15px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--vscode-foreground); font-size: .78em; font-weight: 600; }
        .capture-file-picker { min-width: 0; display: flex; align-items: center; gap: 6px; flex: 1 1 auto; }
        .capture-file-button { flex: 0 0 auto; height: 21px; padding: 0 6px; border: 0; border-radius: 2px; color: var(--vscode-button-foreground); background: var(--vscode-button-background); font: .78em var(--vscode-font-family); cursor: pointer; }
        .capture-file-label { min-width: 0; line-height: 21px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; color: var(--vscode-descriptionForeground); font: .78em var(--vscode-editor-font-family); }
        .realtime-meter-scale { position: relative; height: 8px; color: var(--vscode-descriptionForeground); font-size: .62em; line-height: 8px; }
        .realtime-meter-scale span { position: absolute; transform: translateX(-50%); white-space: nowrap; }
        .realtime-meter-stack { display: flex; flex-direction: column; gap: 0; }
        .realtime-meter-region { margin-top: auto; }
        .large-meter { position: relative; height: 5px; flex: 0 0 5px; overflow: hidden; border: 1px solid var(--vscode-editorWidget-border, rgba(128,128,128,.3)); border-width: 0 1px 1px; background: var(--vscode-editor-background); }
        .large-meter-fill { position: absolute; top: 0; bottom: 0; left: 0; background: var(--vscode-charts-green); }
        .large-meter-grid { position: absolute; inset: 0; pointer-events: none; }
        .large-meter-tick { position: absolute; top: 0; bottom: 0; width: 1px; background: var(--vscode-editorWidget-border, rgba(128,128,128,.4)); }
        .large-meter-tick span { position: absolute; top: 2px; left: 3px; color: var(--vscode-descriptionForeground); font-size: .68em; white-space: nowrap; }
        .meter-menu { position: fixed; z-index: 20; padding: 3px; min-width: 130px; background: var(--vscode-menu-background); border: 1px solid var(--vscode-menu-border, var(--vscode-editorWidget-border)); box-shadow: 0 2px 10px rgba(0,0,0,.35); }
        .meter-menu button { display: block; width: 100%; border: 0; padding: 5px 9px; text-align: left; color: var(--vscode-menu-foreground); background: transparent; cursor: pointer; }
        .meter-menu button:hover { background: var(--vscode-menu-selectionBackground); color: var(--vscode-menu-selectionForeground); }
        .lane-rename-backdrop { position: fixed; z-index: 40; inset: 0; display: grid; place-items: center; background: rgba(0,0,0,.32); }
        .lane-rename-dialog { min-width: 260px; padding: 12px; border: 1px solid var(--vscode-editorWidget-border); background: var(--vscode-editorWidget-background); box-shadow: 0 8px 24px rgba(0,0,0,.45); }
        .lane-rename-dialog label { display: block; margin-bottom: 7px; color: var(--vscode-foreground); }
        .lane-rename-dialog input { box-sizing: border-box; width: 100%; margin-bottom: 10px; color: var(--vscode-input-foreground); background: var(--vscode-input-background); border: 1px solid var(--vscode-input-border, var(--vscode-editorWidget-border)); }
        .lane-rename-actions { display: flex; justify-content: flex-end; gap: 6px; }
        .lane-rename-actions button { border: 0; padding: 4px 9px; color: var(--vscode-button-foreground); background: var(--vscode-button-background); cursor: pointer; }
        .lane-rename-actions button.secondary { color: var(--vscode-button-secondaryForeground); background: var(--vscode-button-secondaryBackground); }

        .connection-row {
            padding-left: 16px;
            gap: 5px;
        }

        .connection-section {
            flex: 0 0 auto;
            border-top: 1px solid var(--vscode-sideBarSectionHeader-border);
        }

        .connection-toggle {
            cursor: pointer;
        }

        .connection-list {
            max-height: 120px;
            overflow: auto;
        }

        .connection-row.overridden {
            color: var(--vscode-descriptionForeground);
        }

        .connection-row.active .connection-state {
            color: var(--vscode-terminal-ansiGreen);
        }

        .connection-row.overridden .connection-state {
            color: var(--vscode-terminal-ansiYellow);
        }

        .connection-arrow {
            color: var(--vscode-descriptionForeground);
        }

        .connection-label {
            overflow: hidden;
            text-overflow: ellipsis;
        }

        .connection-state {
            flex: 0 0 auto;
            margin-left: auto;
            font-variant-numeric: tabular-nums;
        }
    </style>
</head>
<body>
    <div id="root"></div>
    <script nonce="${nonce}">
        const vscode = acquireVsCodeApi();
        const root = document.getElementById("root");
        const laneHeight = 58;
        const laneSettingsPanelHeight = 32;
        const laneHeaderWidth = 96;
        const restoredState = vscode.getState() || {};
        const state = {
            laneViewId: typeof restoredState.laneViewId === "string" ? restoredState.laneViewId : "",
            laneQuery: typeof restoredState.laneQuery === "string" ? restoredState.laneQuery : "",
            laneQuerySchema: null,
            selectedLaneId: typeof restoredState.selectedLaneId === "string" ? restoredState.selectedLaneId : "",
            selectedLaneIds: new Set(Array.isArray(restoredState.selectedLaneIds)
                ? restoredState.selectedLaneIds.filter((laneId) => typeof laneId === "string")
                : (typeof restoredState.selectedLaneId === "string" && restoredState.selectedLaneId
                    ? [restoredState.selectedLaneId] : [])),
            selectionAnchorLaneId: typeof restoredState.selectionAnchorLaneId === "string"
                ? restoredState.selectionAnchorLaneId : "",
            selectionAnchorViewId: typeof restoredState.selectionAnchorViewId === "string"
                ? restoredState.selectionAnchorViewId : "",
            laneOrder: Array.isArray(restoredState.laneOrder)
                ? restoredState.laneOrder.filter((laneId) => typeof laneId === "string") : [],
            startIndex: Number(restoredState.startIndex || 0),
            visibleLaneCount: Number(restoredState.visibleLaneCount || 0),
            totalLaneCount: 0,
            lanes: [],
            connections: [],
            contentByLaneId: {},
            uiStateByLaneId: {},
            playbackSampleIndex: null,
            samplesPerPixel: Number(restoredState.samplesPerPixel || 256),
            panSamples: Number(restoredState.panSamples || 0),
            compiledLaneHeight: Number(restoredState.compiledLaneHeight || 58),
            meterGrid: restoredState.meterGrid === "sample" ? "sample" : "decibel",
            connectionsExpanded: Boolean(restoredState.connectionsExpanded),
            queryExpanded: Boolean(restoredState.queryExpanded),
            expandedLaneSettingsIds: new Set(
                Array.isArray(restoredState.expandedLaneSettingsIds)
                    ? restoredState.expandedLaneSettingsIds.filter((laneId) => typeof laneId === "string")
                    : typeof restoredState.expandedLaneSettingsId === "string"
                        ? [restoredState.expandedLaneSettingsId] : []),
        };
        const lanePresentationPlugins = new Map([${lanePluginRegistrations}].map((plugin) => [plugin.typeId, plugin]));
        function realtimeMeterPosition(level) {
            const linear = Math.max(0, Number(level || 0));
            if (state.meterGrid !== "decibel") return Math.min(1, linear);
            const meterMaximum = Math.pow(10, 4 / 20);
            const xMin = Math.pow(10, -60 / 20) / meterMaximum;
            const s = Math.sqrt(xMin);
            const normalized = Math.min(1, Math.max(xMin, linear / meterMaximum));
            return Math.log((normalized + s) / (s * (1 + s))) / Math.log(1 / s);
        }
        function appendRealtimeMeters(face, lane, content) {
            const eventCount = Number(content?.eventCount || 0);
            const peakLevel = content?.peakLevel;
            const isStereo = (content?.sampleChannelType || lane.sampleChannelType) === "stereo";
            const createMeter = (channel, level) => {
                const meter = document.createElement("div");
                meter.className = "large-meter";
                meter.dataset.meterChannel = channel;
                const fill = document.createElement("div");
                fill.className = "large-meter-fill";
                fill.style.width = String(realtimeMeterPosition(level) * 100) + "%";
                meter.appendChild(fill);
                const grid = document.createElement("div");
                grid.className = "large-meter-grid";
                const tickValues = state.meterGrid === "decibel"
                    ? Array.from({ length: 22 }, (_, index) => -60 + index * 3)
                    : [0, .25, .5, .75, 1];
                for (const value of tickValues) {
                    const tick = document.createElement("div");
                    tick.className = "large-meter-tick";
                    tick.style.left = String((state.meterGrid === "decibel"
                        ? realtimeMeterPosition(Math.pow(10, value / 20)) : value) * 100) + "%";
                    grid.appendChild(tick);
                }
                meter.appendChild(grid);
                return meter;
            };
            const scale = document.createElement("div");
            scale.className = "realtime-meter-scale";
            const scaleValues = state.meterGrid === "decibel"
                ? [-30, -24, -18, -12, -6, 0] : [0, .25, .5, .75, 1];
            for (const value of scaleValues) {
                const label = document.createElement("span");
                label.style.left = String((state.meterGrid === "decibel"
                    ? realtimeMeterPosition(Math.pow(10, value / 20)) : value) * 100) + "%";
                label.textContent = state.meterGrid === "decibel"
                    ? (value > 0 ? "+" : "") + String(value) + " dB" : String(value);
                scale.appendChild(label);
            }
            const region = document.createElement("div");
            region.className = "realtime-meter-region";
            region.appendChild(scale);
            const stack = document.createElement("div");
            stack.className = "realtime-meter-stack";
            stack.appendChild(createMeter("left", peakLevel != null ? peakLevel : eventCount / 8));
            if (isStereo) stack.appendChild(createMeter("right", content?.secondaryPeakLevel || 0));
            region.appendChild(stack);
            face.appendChild(region);
        }
        function drawCompiledWaveform(canvas, samples, secondarySamples, firstSampleIndex, lastSampleIndex) {
            const width = Math.max(1, canvas.clientWidth);
            const height = Math.max(1, canvas.clientHeight);
            const ratio = window.devicePixelRatio || 1;
            canvas.width = Math.round(width * ratio);
            canvas.height = Math.round(height * ratio);
            const context = canvas.getContext("2d");
            if (!context) return;
            context.scale(ratio, ratio);
            context.clearRect(0, 0, width, height);
            const styles = getComputedStyle(document.documentElement);
            const windowFirst = Number(firstSampleIndex);
            const windowLast = Number(lastSampleIndex);
            if (!Number.isFinite(windowFirst) || !Number.isFinite(windowLast) || windowLast < windowFirst) return;
            const xStart = (windowFirst - timelineStart()) / state.samplesPerPixel;
            const xSpan = (windowLast - windowFirst) / state.samplesPerPixel;
            const draw = (values, colour) => {
                if (!Array.isArray(values) || values.length === 0) return;
                const middle = height / 2;
                const amplitude = Math.max(1, middle - 1);
                context.beginPath();
                for (let i = 0; i < values.length; ++i) {
                    const x = values.length === 1 ? xStart : xStart + i * xSpan / (values.length - 1);
                    const y = middle - Math.max(-1, Math.min(1, Number(values[i]) || 0)) * amplitude;
                    if (i === 0) context.moveTo(x, y); else context.lineTo(x, y);
                }
                context.strokeStyle = colour;
                context.lineWidth = 1;
                context.stroke();
            };
            draw(samples, styles.getPropertyValue("--vscode-charts-blue").trim() || "#3794ff");
            draw(secondarySamples, styles.getPropertyValue("--vscode-charts-orange").trim() || "#d18616");
        }
        function appendCaptureFilePicker(container, lane) {
            const snapshot = state.uiStateByLaneId[String(lane.laneId)];
            let path = "timeline-capture.wav";
            try {
                const parsed = snapshot ? JSON.parse(snapshot.serializedState) : null;
                if (typeof parsed?.path === "string" && parsed.path.length > 0) path = parsed.path;
            } catch (_) {}
            const picker = document.createElement("div");
            picker.className = "capture-file-picker";
            const choose = document.createElement("button");
            choose.type = "button";
            choose.className = "capture-file-button";
            choose.textContent = "Choose file…";
            choose.addEventListener("click", (event) => {
                event.stopPropagation();
                vscode.postMessage({
                    type: "chooseCaptureFile",
                    laneId: lane.laneId,
                    currentPath: path,
                    expectedRevision: Number(snapshot?.revision || 0) || undefined,
                });
            });
            const pathLabel = document.createElement("div");
            pathLabel.className = "capture-file-label";
            pathLabel.textContent = path;
            pathLabel.title = path;
            picker.appendChild(choose);
            picker.appendChild(pathLabel);
            container.appendChild(picker);
            const mode = document.createElement("select");
            mode.className = "capture-recording-mode";
            let recordingMode = "reset";
            try { recordingMode = JSON.parse(snapshot?.serializedState || "{}").recordingMode || "reset"; } catch (_) {}
            for (const [value, label] of [["reset", "Reset"], ["overwrite", "Overwrite"], ["insert", "Insert"], ["append", "Append"]]) {
                const option = document.createElement("option"); option.value = value; option.textContent = label; mode.appendChild(option);
            }
            mode.value = recordingMode;
            mode.title = "Recording mode";
            mode.addEventListener("change", (event) => {
                event.stopPropagation();
                let next = {};
                try { next = JSON.parse(snapshot?.serializedState || "{}"); } catch (_) {}
                next.recordingMode = mode.value;
                vscode.postMessage({ type: "setLaneUiState", laneId: lane.laneId,
                    serializedState: JSON.stringify(next), expectedRevision: Number(snapshot?.revision || 0) || undefined });
            });
            container.appendChild(mode);
        }
        function appendCaptureRangeMarkers(track, lane) {
            const snapshot = state.uiStateByLaneId[String(lane.laneId)];
            try {
                const parsed = snapshot ? JSON.parse(snapshot.serializedState) : null;
                const start = Number(parsed?.captureStartIndex);
                const frames = Number(parsed?.captureFrameCount);
                if (!Number.isFinite(start) || !(frames > 0)) return;
                for (const [kind, sampleIndex] of [["start", start], ["end", start + frames]]) {
                    const marker = document.createElement("div");
                    marker.className = "capture-range-boundary " + kind;
                    marker.style.left = String((sampleIndex - timelineStart()) / state.samplesPerPixel) + "px";
                    marker.title = "Capture " + kind + " at sample " + String(sampleIndex);
                    track.appendChild(marker);
                }
            } catch (_) {}
        }
        let viewport = null;
        let hasAppliedInitialScrollPosition = false;
        let pendingViewportPost = 0;
        let lastTimelineViewport = "";
        let renderDeferredUntilBeatDragEnd = false;
        function isEditingBeatControl() {
            const active = document.activeElement;
            return active instanceof HTMLInputElement
                && active.dataset.beatLaneId != null && active.dataset.beatField != null;
        }
        function laneStructureKey(lanes, totalLaneCount) {
            return String(totalLaneCount) + "|" + (Array.isArray(lanes) ? lanes.map((lane) => [
                lane.laneId, lane.domain, lane.modelTypeId, lane.title, lane.description,
            ].join("\u0001")).join("\u0002") : "");
        }
        function flushDeferredLaneRender() {
            if (!renderDeferredUntilBeatDragEnd || document.__ivBeatControlDrag?.active || isEditingBeatControl()) return;
            renderDeferredUntilBeatDragEnd = false;
            renderTimelineChrome();
            renderLanes();
        }
        document.addEventListener("ivBeatDragEnded", () => {
            flushDeferredLaneRender();
        });
        document.addEventListener("focusout", () => setTimeout(flushDeferredLaneRender, 0));
        let lastPostedStartIndex = -1;
        let lastPostedVisibleLaneCount = -1;
        let summary = null;
        let timelineRuler = null;
        let spacer = null;
        let laneWindow = null;
        let connectionToggle = null;
        let connectionList = null;
        let inputWheel = null;
        let inputWheelDismiss = null;
        let renameBackdrop = null;
        let laneQueryRegion = null;
        let laneQueryInput = null;
        let laneQueryToggle = null;
        let pendingLaneQueryPost = 0;
        let laneQuerySuggestions = null;
        let pendingLaneQueryCompletion = 0;
        let laneQueryCompletionRequestId = 0;
        let activeLaneQuerySuggestion = 0;
        function isAuthoredLane(lane) {
            return Boolean(lane?.modelTypeId);
        }
        function requestLaneDeletion(laneIds) {
            for (const laneId of laneIds) {
                connectionDebug("webview posting delete laneId=" + laneId);
                vscode.postMessage({ type: "deleteLane", laneId });
            }
        }
        function selectedLaneIds() {
            return [...state.selectedLaneIds];
        }
        function visibleSelectedLaneIds() {
            return selectedLaneIds().filter((laneId) =>
                state.lanes.some((lane) => String(lane.laneId) === laneId));
        }
        function persistSelection() {
            persistLaneViewState({
                selectedLaneId: state.selectedLaneId,
                selectedLaneIds: selectedLaneIds(),
                selectionAnchorLaneId: state.selectionAnchorLaneId,
                selectionAnchorViewId: state.selectionAnchorViewId,
            });
            vscode.postMessage({ type: "laneSelectionChanged", laneIds: selectedLaneIds(),
                primaryLaneId: state.selectedLaneId, anchorLaneId: state.selectionAnchorLaneId,
                anchorViewId: state.selectionAnchorViewId || state.laneViewId });
        }
        function selectLane(laneId, event) {
            const orderedIds = state.lanes.map((lane) => String(lane.laneId));
            const rangeAnchorLaneId = orderedIds.includes(state.selectionAnchorLaneId)
                ? state.selectionAnchorLaneId
                : orderedIds.includes(state.selectedLaneId) ? state.selectedLaneId : "";
            if (event?.shiftKey && rangeAnchorLaneId) {
                const anchorIndex = orderedIds.indexOf(rangeAnchorLaneId);
                const laneIndex = orderedIds.indexOf(laneId);
                if (anchorIndex >= 0 && laneIndex >= 0) {
                    state.selectedLaneIds = new Set(orderedIds.slice(
                        Math.min(anchorIndex, laneIndex), Math.max(anchorIndex, laneIndex) + 1));
                }
            } else if (event?.ctrlKey || event?.metaKey) {
                if (state.selectedLaneIds.has(laneId)) state.selectedLaneIds.delete(laneId);
                else state.selectedLaneIds.add(laneId);
                state.selectionAnchorLaneId = laneId;
                state.selectionAnchorViewId = state.laneViewId;
            } else {
                state.selectedLaneIds = new Set([laneId]);
                state.selectionAnchorLaneId = laneId;
                state.selectionAnchorViewId = state.laneViewId;
            }
            state.selectedLaneId = state.selectedLaneIds.has(laneId)
                ? laneId : (selectedLaneIds()[0] || "");
            persistSelection();
        }
        let laneQueryCompletion = null;
        function connectionDebug(message) {
            vscode.postMessage({ type: "connectionDebug", message });
        }
        function closeInputWheel() {
            inputWheel?.remove();
            inputWheel = null;
            if (inputWheelDismiss) {
                document.removeEventListener("pointerdown", inputWheelDismiss);
                inputWheelDismiss = null;
            }
        }
        function closeRenameDialog() {
            renameBackdrop?.remove();
            renameBackdrop = null;
        }
        function showRenameDialog(lane) {
            closeRenameDialog();
            const backdrop = document.createElement("div");
            backdrop.className = "lane-rename-backdrop";
            const dialog = document.createElement("form");
            dialog.className = "lane-rename-dialog";
            const label = document.createElement("label");
            label.textContent = "Lane name";
            const input = document.createElement("input");
            input.type = "text";
            input.value = typeof lane.metadata?.["lane.name"] === "string"
                ? lane.metadata["lane.name"] : lane.title;
            label.appendChild(input);
            dialog.appendChild(label);
            const actions = document.createElement("div");
            actions.className = "lane-rename-actions";
            const cancel = document.createElement("button");
            cancel.type = "button";
            cancel.className = "secondary";
            cancel.textContent = "Cancel";
            cancel.addEventListener("click", closeRenameDialog);
            const apply = document.createElement("button");
            apply.type = "submit";
            apply.textContent = "Rename";
            actions.append(cancel, apply);
            dialog.appendChild(actions);
            dialog.addEventListener("submit", (event) => {
                event.preventDefault();
                vscode.postMessage({ type: "renameLane", laneId: lane.laneId, name: input.value });
                closeRenameDialog();
            });
            input.addEventListener("keydown", (event) => {
                // Arrow keys edit the text field; they must never become
                // lane-navigation commands while this dialog is open.
                event.stopPropagation();
                if (event.key === "Escape") {
                    event.preventDefault();
                    closeRenameDialog();
                }
            });
            backdrop.addEventListener("pointerdown", (event) => {
                if (event.target === backdrop) closeRenameDialog();
            });
            backdrop.appendChild(dialog);
            document.body.appendChild(backdrop);
            renameBackdrop = backdrop;
            requestAnimationFrame(() => {
                input.focus();
                input.select();
            });
        }
        function showInputWheel(anchor, inputs, onSelect) {
            closeInputWheel();
            const rect = anchor.getBoundingClientRect();
            const wheel = document.createElement("div");
            wheel.className = "lane-input-wheel";
            wheel.style.left = String(rect.left + rect.width / 2) + "px";
            wheel.style.top = String(rect.top + rect.height / 2) + "px";
            const radius = 58;
            inputs.forEach((input, index) => {
                // The first compatible input begins directly above the
                // button; additional inputs continue clockwise.
                const angle = -Math.PI / 2 + (Math.PI * 2 * index / inputs.length);
                const option = document.createElement("button");
                option.type = "button";
                option.className = "lane-input-wheel-option";
                option.style.left = String(Math.cos(angle) * radius) + "px";
                option.style.top = String(Math.sin(angle) * radius) + "px";
                option.textContent = input.name || input.kind + " " + String(input.ordinal);
                option.title = "Connect to " + option.textContent;
                option.addEventListener("click", (event) => {
                    event.stopPropagation();
                    closeInputWheel();
                    onSelect(input);
                });
                wheel.appendChild(option);
            });
            inputWheel = wheel;
            document.body.appendChild(wheel);
            setTimeout(() => {
                inputWheelDismiss = function dismiss(event) {
                if (wheel.contains(event.target) || event.target === anchor) return;
                closeInputWheel();
                };
                document.addEventListener("pointerdown", inputWheelDismiss);
            }, 0);
        }
        function applyLaneOrder(lanes) {
            const lanesById = new Map(lanes.map((lane) => [String(lane.laneId), lane]));
            const ordered = [];
            // Keep entries that are currently filtered out too, so changing
            // a filter does not discard the view's manual arrangement.
            const retainedIds = [...new Set(state.laneOrder)];
            for (const laneId of state.laneOrder) {
                const lane = lanesById.get(laneId);
                if (!lane) continue;
                ordered.push(lane);
                lanesById.delete(laneId);
            }
            // New filter matches deliberately arrive at the end rather than
            // disturbing a manually established local order.
            for (const lane of lanes) {
                const laneId = String(lane.laneId);
                if (!lanesById.has(laneId)) continue;
                ordered.push(lane);
                retainedIds.push(laneId);
            }
            state.laneOrder = retainedIds;
            return ordered;
        }
        function persistLaneViewState(extra = {}) {
            vscode.setState({ ...vscode.getState(), ...extra,
                laneOrder: state.laneOrder, queryExpanded: state.queryExpanded,
                selectedLaneId: state.selectedLaneId,
                selectedLaneIds: selectedLaneIds(),
                selectionAnchorLaneId: state.selectionAnchorLaneId });
        }
        function syncLaneQueryControl() {
            if (!laneQueryRegion || !laneQueryInput || !laneQueryToggle) return;
            laneQueryRegion.hidden = !state.queryExpanded;
            laneQueryToggle.setAttribute("aria-expanded", String(state.queryExpanded));
            laneQueryToggle.textContent = state.queryExpanded ? "Hide filter" : "Filter";
            if (laneQueryInput.value !== state.laneQuery) laneQueryInput.value = state.laneQuery;
        }
        function postLaneQuery() {
            pendingLaneQueryPost = 0;
            vscode.postMessage({ type: "laneQueryChanged", laneQuery: state.laneQuery });
        }
        function clearLaneQuerySuggestions() {
            ++laneQueryCompletionRequestId;
            laneQueryCompletion = null;
            activeLaneQuerySuggestion = 0;
            laneQuerySuggestions?.remove();
            laneQuerySuggestions = null;
        }
        function utf16OffsetForUtf8ByteOffset(source, targetOffset) {
            let bytes = 0;
            let index = 0;
            for (const character of source) {
                const width = new TextEncoder().encode(character).length;
                if (bytes + width > targetOffset) return index;
                bytes += width;
                index += character.length;
            }
            return source.length;
        }
        function renderLaneQuerySuggestions() {
            laneQuerySuggestions?.remove();
            laneQuerySuggestions = null;
            const candidates = laneQueryCompletion?.candidates;
            if (!laneQueryRegion || !Array.isArray(candidates) || candidates.length === 0) return;
            const list = document.createElement("div");
            list.className = "lane-query-suggestions";
            list.setAttribute("role", "listbox");
            candidates.forEach((candidate, index) => {
                const item = document.createElement("button");
                item.type = "button";
                item.className = "lane-query-suggestion" + (index === activeLaneQuerySuggestion ? " active" : "");
                item.setAttribute("role", "option");
                item.setAttribute("aria-selected", String(index === activeLaneQuerySuggestion));
                const label = document.createElement("span");
                label.textContent = String(candidate.label || candidate.insertText || "");
                const type = document.createElement("span");
                type.className = "lane-query-suggestion-type";
                type.textContent = String(candidate.valueType || candidate.kind || "");
                item.append(label, type);
                item.addEventListener("mousedown", (event) => event.preventDefault());
                item.addEventListener("click", () => applyLaneQuerySuggestion(index));
                list.appendChild(item);
            });
            laneQueryRegion.appendChild(list);
            laneQuerySuggestions = list;
        }
        function scheduleLaneQueryCompletion() {
            if (!laneQueryInput || !state.laneQuerySchema || typeof state.laneQuerySchema.revision !== "number") {
                clearLaneQuerySuggestions();
                return;
            }
            if (pendingLaneQueryCompletion) clearTimeout(pendingLaneQueryCompletion);
            pendingLaneQueryCompletion = setTimeout(() => {
                pendingLaneQueryCompletion = 0;
                const requestId = ++laneQueryCompletionRequestId;
                vscode.postMessage({
                    type: "laneQueryCompletionRequested",
                    requestId,
                    source: laneQueryInput.value,
                    cursorOffset: laneQueryInput.selectionStart || 0,
                    schemaRevision: state.laneQuerySchema.revision,
                });
            }, 50);
        }
        function updateLaneQueryFromInput() {
            state.laneQuery = laneQueryInput.value;
            if (viewport) viewport.scrollTop = 0;
            state.startIndex = 0;
            persistLaneViewState();
            if (pendingLaneQueryPost) clearTimeout(pendingLaneQueryPost);
            pendingLaneQueryPost = setTimeout(postLaneQuery, 160);
            scheduleLaneQueryCompletion();
        }
        function applyLaneQuerySuggestion(index) {
            if (!laneQueryInput || !laneQueryCompletion || !Array.isArray(laneQueryCompletion.candidates)) return;
            const candidate = laneQueryCompletion.candidates[index];
            const range = laneQueryCompletion.replacementRange;
            if (!candidate || !range || typeof candidate.insertText !== "string") return;
            const start = utf16OffsetForUtf8ByteOffset(laneQueryInput.value, Number(range.startOffset || 0));
            const end = utf16OffsetForUtf8ByteOffset(laneQueryInput.value, Number(range.endOffset || 0));
            laneQueryInput.value = laneQueryInput.value.slice(0, start) + candidate.insertText
                + laneQueryInput.value.slice(end);
            const caret = start + candidate.insertText.length;
            laneQueryInput.setSelectionRange(caret, caret);
            clearLaneQuerySuggestions();
            updateLaneQueryFromInput();
        }
        document.addEventListener("keydown", (event) => {
            const active = document.activeElement;
            if (active?.matches?.("input, textarea, select, [contenteditable='true']")
                || active?.isContentEditable) return;
            if (event.key === "Delete" && state.selectedLaneIds.size > 0) {
                const authoredIds = visibleSelectedLaneIds().filter((laneId) =>
                    isAuthoredLane(state.lanes.find((lane) => String(lane.laneId) === laneId)));
                if (authoredIds.length > 0) {
                    event.preventDefault();
                    requestLaneDeletion(authoredIds);
                }
                return;
            }
            if (event.key !== "ArrowUp" && event.key !== "ArrowDown") return;
            if (!Array.isArray(state.lanes) || state.lanes.length === 0) return;

            const currentIndex = state.lanes.findIndex((lane) =>
                String(lane.laneId) === state.selectedLaneId);
            const nextIndex = currentIndex < 0
                ? (event.key === "ArrowUp" ? state.lanes.length - 1 : 0)
                : Math.max(0, Math.min(
                    state.lanes.length - 1,
                    currentIndex + (event.key === "ArrowUp" ? -1 : 1)));
            event.preventDefault();
            if (event.shiftKey && currentIndex >= 0) {
                const selectedIds = new Set(selectedLaneIds());
                if (selectedIds.size === 0) selectedIds.add(state.selectedLaneId);
                if (event.key === "ArrowUp") {
                    for (let index = 0; index < state.lanes.length; ++index) {
                        if (!selectedIds.has(String(state.lanes[index].laneId)) || index === 0
                            || selectedIds.has(String(state.lanes[index - 1].laneId))) continue;
                        [state.lanes[index - 1], state.lanes[index]] = [state.lanes[index], state.lanes[index - 1]];
                    }
                } else {
                    for (let index = state.lanes.length - 1; index >= 0; --index) {
                        if (!selectedIds.has(String(state.lanes[index].laneId)) || index + 1 >= state.lanes.length
                            || selectedIds.has(String(state.lanes[index + 1].laneId))) continue;
                        [state.lanes[index], state.lanes[index + 1]] = [state.lanes[index + 1], state.lanes[index]];
                    }
                }
                state.laneOrder = state.lanes.map((lane) => String(lane.laneId));
                persistLaneViewState();
                renderLanes();
                renderConnections();
                return;
            }
            if (nextIndex === currentIndex) return;
            state.selectedLaneId = String(state.lanes[nextIndex].laneId);
            state.selectedLaneIds = new Set([state.selectedLaneId]);
            state.selectionAnchorLaneId = state.selectedLaneId;
            state.selectionAnchorViewId = state.laneViewId;
            persistSelection();
            renderLanes();
            renderConnections();
            requestAnimationFrame(() => {
                laneWindow?.querySelector('.lane-row[data-lane-id="'
                    + CSS.escape(state.selectedLaneId) + '"]')?.scrollIntoView({ block: "nearest" });
            });
        });

        function laneBaseHeight(lane) {
            return lane?.domain === "compiled" ? state.compiledLaneHeight : laneHeight;
        }
        function laneRenderedHeight(lane) {
            return laneBaseHeight(lane)
                + (state.expandedLaneSettingsIds.has(String(lane?.laneId)) ? laneSettingsPanelHeight : 0);
        }
        function laneVerticalOffset(index) {
            let offset = 0;
            for (let i = 0; i < Math.min(index, state.lanes.length); ++i) offset += laneRenderedHeight(state.lanes[i]);
            return offset;
        }
        function laneIndexAtVerticalOffset(offset) {
            const position = Math.max(0, Number(offset) || 0);
            let cursor = 0;
            for (let i = 0; i < state.lanes.length; ++i) {
                const next = cursor + laneRenderedHeight(state.lanes[i]);
                if (position < next) return i;
                cursor = next;
            }
            return Math.max(0, state.lanes.length - 1);
        }
        function visibleLaneCountAtVerticalOffset(offset, height) {
            const first = laneIndexAtVerticalOffset(offset);
            const limit = Math.max(0, Number(offset) || 0) + Math.max(1, Number(height) || 1);
            let cursor = laneVerticalOffset(first);
            let count = 0;
            for (let i = first; i < state.lanes.length && cursor < limit; ++i) {
                cursor += laneRenderedHeight(state.lanes[i]);
                ++count;
            }
            return Math.max(1, count + 1);
        }
        function totalLanesHeight() {
            return state.lanes.reduce((height, lane) => height + laneRenderedHeight(lane), 0)
                + Math.max(0, state.totalLaneCount - state.lanes.length) * laneHeight;
        }

        function postViewportState() {
            if (!viewport) {
                return;
            }
            const nextStartIndex = laneIndexAtVerticalOffset(viewport.scrollTop);
            const nextVisibleLaneCount = visibleLaneCountAtVerticalOffset(viewport.scrollTop, viewport.clientHeight);
            state.startIndex = nextStartIndex;
            state.visibleLaneCount = nextVisibleLaneCount;
            if (laneWindow) {
                laneWindow.style.transform = "translateY(" + String(laneVerticalOffset(state.startIndex)) + "px)";
            }
            renderTimelineChrome();
            vscode.setState({
                laneViewId: state.laneViewId,
                laneQuery: state.laneQuery,
                selectedLaneId: state.selectedLaneId,
                selectedLaneIds: selectedLaneIds(),
                selectionAnchorLaneId: state.selectionAnchorLaneId,
                startIndex: state.startIndex,
                visibleLaneCount: state.visibleLaneCount,
                connectionsExpanded: state.connectionsExpanded,
                queryExpanded: state.queryExpanded,
                laneOrder: state.laneOrder,
                samplesPerPixel: state.samplesPerPixel,
                panSamples: state.panSamples,
                compiledLaneHeight: state.compiledLaneHeight,
            });
            if (
                nextStartIndex === lastPostedStartIndex
                && nextVisibleLaneCount === lastPostedVisibleLaneCount
            ) {
                return;
            }
            lastPostedStartIndex = nextStartIndex;
            lastPostedVisibleLaneCount = nextVisibleLaneCount;
            vscode.postMessage({
                type: "viewportChanged",
                startIndex: nextStartIndex,
                visibleLaneCount: nextVisibleLaneCount,
            });
        }

        function clampStartIndex(index) {
            const maxStart = Math.max(0, state.totalLaneCount - 1);
            return Math.max(0, Math.min(maxStart, index));
        }

        function moveViewport(deltaRows) {
            if (!viewport || deltaRows === 0) {
                return;
            }
            const currentStartIndex = laneIndexAtVerticalOffset(viewport.scrollTop);
            const nextStartIndex = clampStartIndex(currentStartIndex + deltaRows);
            viewport.scrollTop = laneVerticalOffset(nextStartIndex);
            scheduleViewportPost();
        }

        function scheduleViewportPost() {
            if (pendingViewportPost) {
                return;
            }
            // Scrolling stays entirely local. Batch server window requests so
            // they cannot compete with the browser's scrollbar animation.
            pendingViewportPost = window.setTimeout(() => {
                pendingViewportPost = 0;
                postViewportState();
            }, 50);
        }

        function ensureLayout() {
            if (viewport) {
                return;
            }

            root.textContent = "";
            summary = document.createElement("div");
            summary.className = "timeline-toolbar";
            const heading = document.createElement("strong");
            heading.textContent = "LANES";
            summary.appendChild(heading);
            const range = document.createElement("span");
            range.className = "summary-range";
            summary.appendChild(range);
            laneQueryToggle = document.createElement("button");
            laneQueryToggle.type = "button";
            laneQueryToggle.className = "lane-query-toggle";
            laneQueryToggle.addEventListener("click", () => {
                state.queryExpanded = !state.queryExpanded;
                syncLaneQueryControl();
                persistLaneViewState();
                if (state.queryExpanded) requestAnimationFrame(() => laneQueryInput?.focus());
            });
            summary.appendChild(laneQueryToggle);
            const toolbarSpacer = document.createElement("div");
            toolbarSpacer.className = "spacer";
            summary.appendChild(toolbarSpacer);
            const zoomOut = document.createElement("button");
            zoomOut.className = "zoom-button";
            zoomOut.textContent = "−";
            zoomOut.title = "Zoom out timeline";
            zoomOut.addEventListener("click", () => setZoom(state.samplesPerPixel * 2));
            summary.appendChild(zoomOut);
            const zoomLabel = document.createElement("span");
            zoomLabel.className = "zoom-label";
            summary.appendChild(zoomLabel);
            const zoomIn = document.createElement("button");
            zoomIn.className = "zoom-button";
            zoomIn.textContent = "+";
            zoomIn.title = "Zoom in timeline";
            zoomIn.addEventListener("click", () => setZoom(state.samplesPerPixel / 2));
            summary.appendChild(zoomIn);
            root.appendChild(summary);

            laneQueryRegion = document.createElement("div");
            laneQueryRegion.className = "lane-query-region";
            const queryLabel = document.createElement("label");
            queryLabel.textContent = "Lane filter";
            laneQueryInput = document.createElement("input");
            laneQueryInput.type = "text";
            laneQueryInput.className = "lane-query-input";
            laneQueryInput.placeholder = "query, e.g. dsp_graph.graph_input";
            laneQueryInput.title = "Lane-query expression; leave empty to show all lanes";
            laneQueryInput.setAttribute("aria-label", "Lane filter query");
            queryLabel.htmlFor = "lane-query-input";
            laneQueryInput.id = "lane-query-input";
            laneQueryInput.addEventListener("input", updateLaneQueryFromInput);
            laneQueryInput.addEventListener("focus", scheduleLaneQueryCompletion);
            laneQueryInput.addEventListener("click", scheduleLaneQueryCompletion);
            laneQueryInput.addEventListener("keyup", (event) => {
                if (!["ArrowUp", "ArrowDown", "Enter", "Tab", "Escape"].includes(event.key)) {
                    scheduleLaneQueryCompletion();
                }
            });
            laneQueryInput.addEventListener("blur", () => setTimeout(clearLaneQuerySuggestions, 120));
            laneQueryInput.addEventListener("keydown", (event) => {
                event.stopPropagation();
                const candidates = laneQueryCompletion?.candidates;
                if (Array.isArray(candidates) && candidates.length > 0) {
                    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
                        event.preventDefault();
                        activeLaneQuerySuggestion = (activeLaneQuerySuggestion
                            + (event.key === "ArrowDown" ? 1 : candidates.length - 1)) % candidates.length;
                        renderLaneQuerySuggestions();
                        return;
                    }
                    if (event.key === "Enter" || event.key === "Tab") {
                        event.preventDefault();
                        applyLaneQuerySuggestion(activeLaneQuerySuggestion);
                        return;
                    }
                }
                if (event.key === "Escape") {
                    event.preventDefault();
                    if (laneQueryCompletion) {
                        clearLaneQuerySuggestions();
                        return;
                    }
                    state.queryExpanded = false;
                    syncLaneQueryControl();
                    persistLaneViewState();
                }
            });
            laneQueryRegion.append(queryLabel, laneQueryInput);
            root.appendChild(laneQueryRegion);
            syncLaneQueryControl();

            timelineRuler = document.createElement("div");
            timelineRuler.className = "timeline-ruler";
            timelineNavigation.install(timelineRuler, "ruler");
            root.appendChild(timelineRuler);

            viewport = document.createElement("div");
            viewport.className = "lane-viewport";
            viewport.addEventListener("scroll", scheduleViewportPost);
            // Preserve native vertical scrolling for the lane list. Rulers
            // themselves install the full timeline-control wheel behavior.

            spacer = document.createElement("div");
            spacer.className = "lane-spacer";
            spacer.style.height = String(totalLanesHeight()) + "px";
            viewport.appendChild(spacer);

            laneWindow = document.createElement("div");
            laneWindow.className = "lane-window";
            viewport.appendChild(laneWindow);
            root.appendChild(viewport);

            const connectionSection = document.createElement("div");
            connectionSection.className = "connection-section";
            connectionToggle = document.createElement("div");
            connectionToggle.className = "section-title connection-toggle";
            connectionToggle.addEventListener("click", () => {
                state.connectionsExpanded = !state.connectionsExpanded;
                vscode.setState({
                    startIndex: state.startIndex,
                    visibleLaneCount: state.visibleLaneCount,
                    connectionsExpanded: state.connectionsExpanded,
                    selectedLaneId: state.selectedLaneId,
                    selectedLaneIds: selectedLaneIds(),
                    selectionAnchorLaneId: state.selectionAnchorLaneId,
                    laneOrder: state.laneOrder,
                    samplesPerPixel: state.samplesPerPixel,
                    panSamples: state.panSamples,
                });
                renderConnections();
            });
            connectionSection.appendChild(connectionToggle);

            connectionList = document.createElement("div");
            connectionList.className = "connection-list";
            connectionSection.appendChild(connectionList);
            // Connections are represented by selected-lane cables only; do
            // not reserve a bottom panel for the legacy list.
        }

        function setZoom(samplesPerPixel) {
            state.samplesPerPixel = Math.max(1, Math.min(65536, Math.round(samplesPerPixel)));
            vscode.setState({
                startIndex: state.startIndex,
                visibleLaneCount: state.visibleLaneCount,
                connectionsExpanded: state.connectionsExpanded,
                selectedLaneId: state.selectedLaneId,
                selectedLaneIds: selectedLaneIds(),
                selectionAnchorLaneId: state.selectionAnchorLaneId,
                laneOrder: state.laneOrder,
                queryExpanded: state.queryExpanded,
                samplesPerPixel: state.samplesPerPixel,
                panSamples: state.panSamples,
                compiledLaneHeight: state.compiledLaneHeight,
            });
            renderLanes();
            renderTimelineChrome();
            postTimelineViewport();
        }

        // Preserve the timeline sample under the pointer while zooming. A
        // ruler may start at the pre-zero gutter or at the timeline origin.
        function setZoomAt(samplesPerPixel, clientX, bounds, sampleAtLeft, includesLaneHeader) {
            const oldSamplesPerPixel = state.samplesPerPixel;
            const sampleAtPointer = sampleAtLeft
                + (clientX - bounds.left) * oldSamplesPerPixel;
            const nextSamplesPerPixel = Math.max(1, Math.min(65536, Math.round(samplesPerPixel)));
            state.panSamples = sampleAtPointer
                - (clientX - bounds.left) * nextSamplesPerPixel
                + (includesLaneHeader ? laneHeaderWidth * nextSamplesPerPixel : 0);
            setZoom(nextSamplesPerPixel);
        }

        function installTimelineControls(element, sampleAtLeft, includesLaneHeader) {
            let panStartX = null;
            let panStartSamples = 0;
            let scrubPointerId = null;
            const sampleAtEvent = (event) => sampleAtLeft()
                + (event.clientX - element.getBoundingClientRect().left) * state.samplesPerPixel;
            const finishPointer = (event) => {
                if (scrubPointerId === event.pointerId) scrubPointerId = null;
                let wasPanning = false;
                if (panStartX != null) {
                    panStartX = null;
                    wasPanning = true;
                    vscode.setState({ ...vscode.getState(), panSamples: state.panSamples });
                }
                try { element.releasePointerCapture(event.pointerId); } catch (_) {}
                // A beat-grid pan keeps its captured element alive while the
                // pointer moves. Rebuild every lane only after that gesture
                // finishes, once pointer capture is no longer needed.
                if (wasPanning) renderLanes();
            };
            element.addEventListener("pointerdown", (event) => {
                if (event.button !== 0) return;
                element.setPointerCapture(event.pointerId);
                if (!event.shiftKey) {
                    scrubPointerId = event.pointerId;
                    scrubToSample(sampleAtEvent(event));
                    return;
                }
                panStartX = event.clientX;
                panStartSamples = state.panSamples;
            });
            element.addEventListener("pointermove", (event) => {
                if (scrubPointerId === event.pointerId) {
                    scrubToSample(sampleAtEvent(event));
                    return;
                }
                if (panStartX == null) return;
                state.panSamples = panStartSamples - (event.clientX - panStartX) * state.samplesPerPixel;
                renderTimelineChrome();
                for (const grid of laneWindow.querySelectorAll(".beat-event-grid")) {
                    grid.__ivRefreshBeats?.();
                }
                postTimelineViewport();
            });
            element.addEventListener("pointerup", finishPointer);
            element.addEventListener("pointercancel", finishPointer);
            element.addEventListener("wheel", (event) => {
                if (event.altKey) {
                    event.preventDefault();
                    setVerticalZoom(state.compiledLaneHeight * (event.deltaY > 0 ? 0.85 : 1.18));
                } else if (event.ctrlKey || event.metaKey) {
                    event.preventDefault();
                    setZoomAt(
                        state.samplesPerPixel * (event.deltaY > 0 ? 1.25 : 0.8),
                        event.clientX,
                        element.getBoundingClientRect(),
                        sampleAtLeft(),
                        includesLaneHeader);
                } else if (event.deltaX !== 0 || event.shiftKey) {
                    event.preventDefault();
                    const horizontalDelta = event.deltaX !== 0 ? event.deltaX : event.deltaY;
                    state.panSamples += horizontalDelta * state.samplesPerPixel;
                    vscode.setState({ ...vscode.getState(), panSamples: state.panSamples });
                    renderTimelineChrome();
                    renderLanes();
                    postTimelineViewport();
                }
            }, { passive: false });
        }

        // One navigation surface for the ruler and every compiled lane. Lane
        // presentations only choose their coordinate origin; scrubbing,
        // panning, horizontal zoom and vertical zoom remain identical.
        const timelineNavigation = {
            install(element, origin) {
                const isRuler = origin === "ruler";
                installTimelineControls(element,
                    () => isRuler ? rulerStart() : timelineStart(), isRuler);
            },
            timelineStart,
            rulerStart,
            scrubToSample,
        };

        function setVerticalZoom(height) {
            state.compiledLaneHeight = Math.max(34, Math.min(220, Math.round(height)));
            vscode.setState({
                startIndex: state.startIndex,
                visibleLaneCount: state.visibleLaneCount,
                connectionsExpanded: state.connectionsExpanded,
                selectedLaneId: state.selectedLaneId,
                selectedLaneIds: selectedLaneIds(),
                selectionAnchorLaneId: state.selectionAnchorLaneId,
                laneOrder: state.laneOrder,
                samplesPerPixel: state.samplesPerPixel,
                panSamples: state.panSamples,
                compiledLaneHeight: state.compiledLaneHeight,
            });
            renderLanes();
        }

        function showMeterMenu(event, lane) {
            event.preventDefault();
            event.stopPropagation();
            document.querySelectorAll(".meter-menu").forEach((menu) => menu.remove());
            const menu = document.createElement("div");
            menu.className = "meter-menu";
            for (const [value, label] of [["decibel", "Decibel grid"], ["sample", "Sample-value grid"]]) {
                const button = document.createElement("button");
                button.textContent = (state.meterGrid === value ? "✓ " : "") + label;
                button.addEventListener("click", () => {
                    state.meterGrid = value;
                    vscode.setState({ ...vscode.getState(), meterGrid: state.meterGrid });
                    menu.remove();
                    renderLanes();
                });
                menu.appendChild(button);
            }
            const rename = document.createElement("button");
            rename.textContent = "Rename lane…";
            rename.addEventListener("click", () => {
                menu.remove();
                showRenameDialog(lane);
            });
            menu.appendChild(rename);
            menu.style.left = String(event.clientX) + "px";
            menu.style.top = String(event.clientY) + "px";
            document.body.appendChild(menu);
            // Do not remove the menu on the button's own pointer-down: doing
            // so detaches the button before its click handler can apply the
            // selected meter scale.
            setTimeout(() => document.addEventListener("pointerdown", (pointerEvent) => {
                if (!menu.contains(pointerEvent.target)) menu.remove();
            }, { once: true }), 0);
        }

        function showLaneContextMenu(event, lane) {
            event.preventDefault();
            event.stopPropagation();
            if (!state.selectedLaneIds.has(String(lane.laneId))) selectLane(String(lane.laneId));
            document.querySelectorAll(".meter-menu").forEach((menu) => menu.remove());
            const menu = document.createElement("div");
            menu.className = "meter-menu";
            const rename = document.createElement("button");
            rename.textContent = "Rename lane…";
            rename.addEventListener("click", () => {
                menu.remove();
                showRenameDialog(lane);
            });
            menu.appendChild(rename);
            if (isAuthoredLane(lane)) {
                const duplicate = document.createElement("button");
                const selectedAuthoredIds = visibleSelectedLaneIds().filter((laneId) =>
                    isAuthoredLane(state.lanes.find((candidate) => String(candidate.laneId) === laneId)));
                duplicate.textContent = selectedAuthoredIds.length > 1 ? "Duplicate selected lanes" : "Duplicate lane";
                duplicate.addEventListener("click", () => {
                    menu.remove();
                    for (const laneId of selectedAuthoredIds) {
                        vscode.postMessage({ type: "duplicateLane", laneId });
                    }
                });
                menu.appendChild(duplicate);
                const remove = document.createElement("button");
                remove.textContent = selectedAuthoredIds.length > 1 ? "Delete selected lanes" : "Delete lane";
                remove.addEventListener("click", () => {
                    connectionDebug("webview context-menu delete clicked laneIds=" + selectedAuthoredIds.join(","));
                    menu.remove();
                    requestLaneDeletion(selectedAuthoredIds);
                });
                menu.appendChild(remove);
            }
            menu.style.left = String(event.clientX) + "px";
            menu.style.top = String(event.clientY) + "px";
            document.body.appendChild(menu);
            setTimeout(() => document.addEventListener("pointerdown", (pointerEvent) => {
                if (!menu.contains(pointerEvent.target)) menu.remove();
            }, { once: true }), 0);
        }

        function timelineStart() {
            return Math.round(state.panSamples);
        }

        function playheadX() {
            return (Number(state.playbackSampleIndex || 0) - timelineStart()) / state.samplesPerPixel;
        }

        function scrubToSample(sampleIndex) {
            sampleIndex = Math.max(0, Math.round(sampleIndex));
            state.playbackSampleIndex = sampleIndex;
            vscode.postMessage({ type: "scrubPlayback", sampleIndex });
            renderTimelineChrome();
            // Do not replace rows while a ruler owns pointer capture. In
            // particular, a beat-grid scrub must retain its grid through the
            // whole drag so every pointer move can update the playhead.
            updateDynamicContent();
        }

        function rulerStart() {
            return timelineStart() - laneHeaderWidth * state.samplesPerPixel;
        }

        function postTimelineViewport() {
            if (!viewport) return;
            // Lane content begins after the fixed header. Request only that
            // region so the sample-window origin and every lane canvas agree.
            const trackWidth = Math.max(1, viewport.clientWidth - laneHeaderWidth);
            const visibleFirstSampleIndex = Math.floor(timelineStart());
            const visibleLastSampleIndex = Math.ceil(timelineStart() + trackWidth * state.samplesPerPixel);
            const firstSampleIndex = Math.max(0, visibleFirstSampleIndex);
            const lastSampleIndex = Math.max(firstSampleIndex, visibleLastSampleIndex);
            // A negative pan has a pre-zero gutter. Sample only the portion
            // that intersects the timeline; drawing uses the returned bounds
            // to place that portion at its real x coordinate.
            const displaySampleCount = Math.max(1, Math.ceil(
                Math.max(0, visibleLastSampleIndex - firstSampleIndex) / state.samplesPerPixel));
            const next = [firstSampleIndex, lastSampleIndex, displaySampleCount].join(":");
            if (next === lastTimelineViewport) return;
            lastTimelineViewport = next;
            vscode.postMessage({ type: "timelineViewportChanged", firstSampleIndex, lastSampleIndex, displaySampleCount });
        }

        function renderTimelineChrome() {
            if (!summary || !timelineRuler) return;
            const range = summary.querySelector(".summary-range");
            const zoomLabel = summary.querySelector(".zoom-label");
            if (range) range.textContent = String(state.startIndex) + "–" + String(state.startIndex + state.lanes.length) + " / " + String(state.totalLaneCount);
            if (zoomLabel) zoomLabel.textContent = String(state.samplesPerPixel) + " samples/px · " + String(state.compiledLaneHeight) + " px";
            timelineRuler.textContent = "";
            const width = Math.max(1, timelineRuler.clientWidth);
            const samplesPerSecond = 48000;
            const pixelsPerSecond = samplesPerSecond / state.samplesPerPixel;
            const start = rulerStart();
            const rulerSteps = [1, 2, 5, 10, 20, 30, 60, 120, 300, 600, 1200, 3600];
            const secondsPerGraduation = rulerSteps.find((step) => step * pixelsPerSecond >= 72)
                || rulerSteps[rulerSteps.length - 1];
            const firstSecond = Math.max(0, Math.ceil(start / samplesPerSecond / secondsPerGraduation) * secondsPerGraduation);
            const lastSecond = Math.ceil((start + width * state.samplesPerPixel) / samplesPerSecond);
            for (let second = firstSecond; second <= lastSecond; second += secondsPerGraduation) {
                const x = (second * samplesPerSecond - start) / state.samplesPerPixel;
                const tick = document.createElement("div");
                tick.className = "timeline-tick";
                tick.style.left = String(x) + "px";
                tick.textContent = String(second) + " s";
                timelineRuler.appendChild(tick);
            }
            const cursor = document.createElement("div");
            cursor.className = "playhead";
            cursor.style.left = String(laneHeaderWidth + playheadX()) + "px";
            cursor.title = "playback sample " + String(state.playbackSampleIndex ?? 0);
            timelineRuler.appendChild(cursor);
        }

        // Content updates are published continuously while the timeline runs. It
        // deliberately updates only the pieces whose values came from that
        // message; recreating every row (and every beat control) here made the
        // editor needlessly unstable and expensive.
        function updateDynamicContent() {
            for (const lane of state.lanes) {
                const laneId = String(lane.laneId);
                const row = laneWindow.querySelector('.lane-row[data-lane-id="' + CSS.escape(laneId) + '"]');
                if (!row) continue;
                const content = state.contentByLaneId[laneId];
                const eventCount = Number(content?.eventCount || 0);
                const peakLevel = content?.peakLevel;

                const labelMeter = row.querySelector(".lane-meter");
                if (labelMeter) {
                    labelMeter.classList.toggle("events", eventCount > 0);
                    labelMeter.title = peakLevel != null
                        ? "peak " + Number(peakLevel).toFixed(3)
                        : eventCount > 0 ? String(eventCount) + " events" : "realtime level: 0.000";
                    const fill = labelMeter.querySelector(".lane-meter-fill");
                    if (fill) {
                        const level = peakLevel != null
                            ? Math.max(0, Math.min(1, Number(peakLevel)))
                            : Math.min(1, eventCount / 8);
                        fill.style.width = String(level * 100) + "%";
                    }
                }

                const waveform = row.querySelector(".compiled-waveform");
                if (waveform) {
                    drawCompiledWaveform(waveform, content?.samples || [], content?.secondarySamples || [],
                        content?.sampleWindowFirstIndex, content?.sampleWindowLastIndex);
                }

                const largeMeters = row.querySelectorAll(".large-meter");
                if (largeMeters.length > 0) {
                    for (const meter of largeMeters) {
                        const level = meter.dataset.meterChannel === "right" ? content?.secondaryPeakLevel : peakLevel;
                        const fill = meter.querySelector(".large-meter-fill");
                        if (fill) fill.style.width = String(realtimeMeterPosition(level) * 100) + "%";
                    }
                }

                const beatGrid = row.querySelector(".beat-event-grid");
                if (beatGrid) {
                    // Apply authoritative output to the grid only. The beat
                    // controls retain their independent user-input state.
                    const snapshot = state.uiStateByLaneId[laneId];
                    try {
                        const settings = snapshot ? JSON.parse(snapshot.serializedState) : null;
                        if (settings && typeof settings === "object") {
                            beatGrid.__ivApplyBeatSettings?.(settings, snapshot.revision);
                        }
                    } catch (_) {}
                    beatGrid.__ivRefreshBeats?.();
                }
            }
            const rulerPlayhead = timelineRuler.querySelector(".playhead");
            if (rulerPlayhead) {
                rulerPlayhead.style.left = String(laneHeaderWidth + playheadX()) + "px";
                rulerPlayhead.title = "playback sample " + String(state.playbackSampleIndex ?? 0);
            }
            const canvasPlayhead = laneWindow.querySelector(".canvas-playhead");
            if (canvasPlayhead) {
                canvasPlayhead.style.left = String(laneHeaderWidth + playheadX()) + "px";
                canvasPlayhead.title = "playback sample " + String(state.playbackSampleIndex ?? 0);
            }
        }

        // The server remains the authority for graph validation. This mirrors
        // its lane-level reachability check so the connection control does not
        // offer an edge that can only close a directed cycle.
        function wouldCreateLaneCycle(sourceLaneId, targetLaneId) {
            if (sourceLaneId === targetLaneId) return true;
            const outputsByLaneId = new Map();
            for (const connection of state.connections || []) {
                const source = String(connection.sourceLaneId);
                const target = String(connection.targetLaneId);
                let outputs = outputsByLaneId.get(source);
                if (!outputs) {
                    outputs = [];
                    outputsByLaneId.set(source, outputs);
                }
                outputs.push(target);
            }
            const pending = [String(targetLaneId)];
            const visited = new Set();
            while (pending.length > 0) {
                const current = pending.pop();
                if (current === String(sourceLaneId)) return true;
                if (visited.has(current)) continue;
                visited.add(current);
                for (const next of outputsByLaneId.get(current) || []) pending.push(next);
            }
            return false;
        }

        function renderLanes() {
            const activeElement = document.activeElement;
            const focusedBeatControl = activeElement instanceof HTMLInputElement
                && activeElement.dataset.beatLaneId && activeElement.dataset.beatField
                ? {
                    laneId: activeElement.dataset.beatLaneId,
                    field: activeElement.dataset.beatField,
                    selectionStart: activeElement.selectionStart,
                    selectionEnd: activeElement.selectionEnd,
                }
                : null;
            laneWindow.textContent = "";
            spacer.style.height = String(totalLanesHeight()) + "px";
            laneWindow.style.transform = "translateY(" + String(laneVerticalOffset(state.startIndex)) + "px)";

            for (const lane of state.lanes) {
                const row = document.createElement("div");
                row.className = "lane-row " + lane.domain + (state.selectedLaneIds.has(String(lane.laneId)) ? " selected" : "");
                row.dataset.laneId = String(lane.laneId);
                const baseHeight = laneBaseHeight(lane);
                row.style.setProperty("--lane-content-height", String(baseHeight) + "px");
                row.style.height = String(baseHeight
                    + (state.expandedLaneSettingsIds.has(String(lane.laneId)) ? laneSettingsPanelHeight : 0)) + "px";
                row.addEventListener("click", (event) => {
                    // Interactive lane presentations must not trigger row
                    // selection/rebuilds. Native number steppers otherwise
                    // change once and are immediately recreated at defaults.
                    if (event.target.closest(".lane-connection-button, input, button, select, textarea, label")) return;
                    selectLane(String(lane.laneId), event);
                    renderLanes();
                    renderConnections();
                });
                row.addEventListener("contextmenu", (event) => {
                    showLaneContextMenu(event, lane);
                });

                const connectionButton = document.createElement("button");
                connectionButton.type = "button";
                connectionButton.className = "lane-connection-button";
                if (!state.selectedLaneId) {
                    connectionButton.textContent = "•";
                    connectionButton.disabled = true;
                    connectionButton.title = "Select the lane by clicking its row";
                } else if (state.selectedLaneIds.has(String(lane.laneId))) {
                    connectionButton.textContent = "•";
                    connectionButton.classList.add("selected");
                    connectionButton.disabled = true;
                    connectionButton.title = state.selectedLaneIds.size === 1 ? "Selected lane" : "Selected lane (batch source)";
                } else {
                    const sources = visibleSelectedLaneIds().map((laneId) => state.lanes.find((candidate) =>
                        String(candidate.laneId) === laneId)).filter(Boolean);
                    const targetLaneId = String(lane.laneId);
                    const existingConnections = (state.connections || []).filter((connection) =>
                        state.selectedLaneIds.has(connection.sourceLaneId) && connection.targetLaneId === targetLaneId);
                    const compatibleInputs = (lane.inputs || []).filter((input) => sources.some((source) =>
                        input.kind === (source.outputKind || "sample")
                        && !wouldCreateLaneCycle(String(source.laneId), targetLaneId)));
                    const connectableInputs = compatibleInputs.filter((input) => sources.some((source) =>
                        input.kind === (source.outputKind || "sample")
                        && !wouldCreateLaneCycle(String(source.laneId), targetLaneId)
                        && !(state.connections || []).some((connection) =>
                            connection.sourceLaneId === String(source.laneId)
                            && connection.targetLaneId === targetLaneId
                            && connection.portDomain === input.domain
                            && connection.portKind === input.kind
                            && connection.portOrdinal === input.ordinal)));
                    const allConnected = sources.length > 0 && sources.every((source) =>
                        (state.connections || []).some((connection) =>
                            connection.sourceLaneId === String(source.laneId)
                            && connection.targetLaneId === targetLaneId));
                    connectionButton.textContent = allConnected ? "−" : "+";
                    connectionButton.disabled = allConnected ? existingConnections.length === 0 : connectableInputs.length === 0;
                    connectionButton.title = allConnected
                        ? "Disconnect selected lanes from this lane"
                        : connectableInputs.length > 0
                            ? "Connect selected lanes to " + (connectableInputs[0].name || connectableInputs[0].kind)
                                + (connectableInputs.length > 1 ? " • hold: choose input" : "")
                            : compatibleInputs.length > 0
                                ? "Selected lanes are already connected or would create a cycle"
                                : "Selected lanes have no compatible output for this lane";
                    const connectToInput = (input) => {
                        for (const source of sources) {
                            const sourceLaneId = String(source.laneId);
                            if (input.kind !== (source.outputKind || "sample")
                                || wouldCreateLaneCycle(sourceLaneId, targetLaneId)
                                || (state.connections || []).some((connection) => connection.sourceLaneId === sourceLaneId
                                    && connection.targetLaneId === targetLaneId && connection.portDomain === input.domain
                                    && connection.portKind === input.kind && connection.portOrdinal === input.ordinal)) continue;
                            connectionDebug("button connect " + sourceLaneId + " -> " + targetLaneId
                                + " " + input.domain + "/" + input.kind + "[" + String(input.ordinal) + "]");
                            vscode.postMessage({ type: "connectLanes", sourceLaneId, targetLaneId,
                                portDomain: input.domain, portKind: input.kind, portOrdinal: input.ordinal });
                        }
                    };
                    const disconnect = () => {
                        for (const connection of existingConnections) {
                            connectionDebug("button disconnect " + connection.sourceLaneId + " -> " + connection.targetLaneId);
                            vscode.postMessage({ type: "disconnectLanes", sourceLaneId: connection.sourceLaneId,
                                targetLaneId: connection.targetLaneId, portDomain: connection.portDomain,
                                portKind: connection.portKind, portOrdinal: connection.portOrdinal });
                        }
                    };
                    let holdTimer = null;
                    let openedInputWheel = false;
                    connectionButton.addEventListener("pointerdown", (event) => {
                        event.stopPropagation();
                        if (allConnected) {
                            return;
                        }
                        if (connectableInputs.length < 2) return;
                        openedInputWheel = false;
                        holdTimer = setTimeout(() => {
                            openedInputWheel = true;
                            showInputWheel(connectionButton, connectableInputs, connectToInput);
                        }, 360);
                    });
                    connectionButton.addEventListener("pointerup", (event) => {
                        event.stopPropagation();
                        if (holdTimer) clearTimeout(holdTimer);
                        holdTimer = null;
                        if (openedInputWheel) return;
                        if (allConnected) disconnect();
                        else if (connectableInputs.length > 0) connectToInput(connectableInputs[0]);
                    });
                    connectionButton.addEventListener("pointercancel", () => {
                        if (holdTimer) clearTimeout(holdTimer);
                        holdTimer = null;
                    });
                    connectionButton.addEventListener("click", (event) => {
                        // Keyboard activation has no pointer hold; retain the
                        // ordinary first-input action for accessibility.
                        event.stopPropagation();
                        if (event.detail !== 0) return;
                        if (allConnected) disconnect();
                        else if (connectableInputs.length > 0) connectToInput(connectableInputs[0]);
                    });
                }
                row.appendChild(connectionButton);

                const debugCopy = document.createElement("button");
                debugCopy.type = "button";
                debugCopy.className = "lane-debug-copy-button";
                debugCopy.classList.add(lane.domain);
                debugCopy.textContent = "⧉";
                debugCopy.title = "Copy lane debug info";
                debugCopy.setAttribute("aria-label", "Copy lane debug info");
                debugCopy.addEventListener("click", (event) => {
                    event.stopPropagation();
                    const connections = (state.connections || []).filter((connection) =>
                        connection.sourceLaneId === String(lane.laneId)
                        || connection.targetLaneId === String(lane.laneId));
                    vscode.postMessage({
                        type: "copyLaneDebugInfo",
                        text: JSON.stringify({
                            laneId: lane.laneId,
                            domain: lane.domain,
                            outputKind: lane.outputKind,
                            modelTypeId: lane.modelTypeId,
                            metadata: lane.metadata,
                            inputs: lane.inputs,
                            connections,
                        }, null, 2),
                    });
                });
                row.appendChild(debugCopy);

                const label = document.createElement("div");
                label.className = "lane-label";
                const isCaptureLane = lane.modelTypeId === "iv.timeline.audio-file-capture";
                const isBeatTriggerLane = lane.modelTypeId === "iv.timeline.beat-trigger";
                const hasLaneSettings = isCaptureLane || isBeatTriggerLane;
                const settingsAreOpen = state.expandedLaneSettingsIds.has(String(lane.laneId));
                if (settingsAreOpen) row.classList.add("settings-open");

                const title = document.createElement("div");
                title.className = "title";
                title.textContent = lane.title;
                // Compiled lanes put their title directly over their timeline
                // presentation. Realtime lanes retain the title in their
                // realtime face, where it shares the meter layout.

                const content = state.contentByLaneId[String(lane.laneId)];
                const eventCount = Number(content?.eventCount || 0);
                const peakLevel = content?.peakLevel;
                if (lane.domain === "compiled" && content && lane.modelTypeId !== "iv.timeline.beat-trigger" && !isCaptureLane) {
                    const meter = document.createElement("div");
                    meter.className = "lane-meter" + (eventCount > 0 ? " events" : "");
                    meter.title = peakLevel != null
                        ? "peak " + Number(peakLevel).toFixed(3)
                        : eventCount > 0 ? String(eventCount) + " events" : "realtime level: 0.000";
                    const fill = document.createElement("div");
                    fill.className = "lane-meter-fill";
                    const level = peakLevel != null
                        ? Math.max(0, Math.min(1, Number(peakLevel)))
                        : Math.min(1, eventCount / 8);
                    fill.style.width = String(level * 100) + "%";
                    meter.appendChild(fill);
                    label.appendChild(meter);
                }

                row.appendChild(label);
                let settingsPanel = null;
                if (hasLaneSettings) {
                    const settingsToggle = document.createElement("button");
                    settingsToggle.type = "button";
                    settingsToggle.className = "lane-settings-toggle";
                    settingsToggle.textContent = settingsAreOpen ? "▴" : "▾";
                    settingsToggle.setAttribute("aria-expanded", String(settingsAreOpen));
                    settingsToggle.title = settingsAreOpen ? "Hide lane settings" : "Show lane settings";
                    settingsToggle.addEventListener("click", (event) => {
                        event.stopPropagation();
                        const laneId = String(lane.laneId);
                        if (settingsAreOpen) state.expandedLaneSettingsIds.delete(laneId);
                        else state.expandedLaneSettingsIds.add(laneId);
                        persistLaneViewState({ expandedLaneSettingsIds: [...state.expandedLaneSettingsIds] });
                        renderLanes();
                    });
                    row.appendChild(settingsToggle);
                }
                if (hasLaneSettings && settingsAreOpen) {
                    settingsPanel = document.createElement("div");
                    settingsPanel.className = "lane-settings-panel";
                    if (isCaptureLane) appendCaptureFilePicker(settingsPanel, lane);
                    row.appendChild(settingsPanel);
                }
                if (lane.domain !== "compiled") {
                    const staticFace = document.createElement("div");
                    staticFace.className = "realtime-face";
                    staticFace.title = "Right-click to choose decibel or sample-value scale";
                    if (isCaptureLane) {
                        const header = document.createElement("div");
                        header.className = "capture-header";
                        const kind = document.createElement("span");
                        kind.className = "capture-kind";
                        kind.textContent = lane.title;
                        header.appendChild(kind);
                        appendCaptureFilePicker(header, lane);
                        staticFace.appendChild(header);
                    } else {
                        const meterName = document.createElement("div");
                        meterName.className = "realtime-meter-name";
                        meterName.textContent = lane.title;
                        staticFace.appendChild(meterName);
                    }
                    appendRealtimeMeters(staticFace, lane, content);
                    staticFace.addEventListener("contextmenu", (event) => showMeterMenu(event, lane));
                    row.appendChild(staticFace);
                    laneWindow.appendChild(row);
                    continue;
                }
                const presentation = lanePresentationPlugins.get(lane.modelTypeId);
                if (presentation?.render({ lane, content, row, state, laneWindow, timelineStart, rulerStart, scrubToSample,
                    timelineNavigation,
                    settingsContainer: settingsPanel,
                    usesSettingsPanel: hasLaneSettings,
                    postLaneUiState: (laneId, serializedState, expectedRevision) => vscode.postMessage({
                        type: "setLaneUiState", laneId, serializedState, expectedRevision,
                    }),
                    postDebug: (message) => vscode.postMessage(message),
                })) continue;
                const track = document.createElement("div");
                track.className = "lane-track";
                if (isCaptureLane) track.classList.add("capture-track");
                track.style.setProperty("--second-width", String(Math.max(1, 48000 / state.samplesPerPixel)) + "px");
                if (lane.domain === "compiled") {
                    const waveform = document.createElement("canvas");
                    waveform.className = "compiled-waveform";
                    track.appendChild(waveform);
                    requestAnimationFrame(() => drawCompiledWaveform(
                        waveform, content?.samples || [], content?.secondarySamples || [],
                        content?.sampleWindowFirstIndex, content?.sampleWindowLastIndex));
                }
                if (lane.domain === "compiled") {
                    const trackTitle = document.createElement("div");
                    if (isCaptureLane) appendCaptureRangeMarkers(track, lane);
                    trackTitle.className = "compiled-lane-title";
                    trackTitle.textContent = lane.title;
                    trackTitle.title = lane.title;
                    track.appendChild(trackTitle);
                }
                row.appendChild(track);
                timelineNavigation.install(track, "lane");

                laneWindow.appendChild(row);
            }

            const canvasPlayhead = document.createElement("div");
            canvasPlayhead.className = "canvas-playhead";
            canvasPlayhead.style.left = String(laneHeaderWidth + playheadX()) + "px";
            canvasPlayhead.title = "playback sample " + String(state.playbackSampleIndex ?? 0);
            laneWindow.appendChild(canvasPlayhead);
            renderSelectedConnections();

            if (!Array.isArray(state.lanes) || state.lanes.length === 0) {
                const empty = document.createElement("div");
                empty.className = "empty";
                empty.textContent = state.totalLaneCount > 0 ? "[scrolling]" : "[no lanes]";
                laneWindow.appendChild(empty);
            }

            // Content updates replace the event markers, but must not make a
            // number input lose focus midway through an edit or spinner click.
            if (focusedBeatControl) {
                for (const input of laneWindow.querySelectorAll("input[data-beat-lane-id][data-beat-field]")) {
                    if (input.dataset.beatLaneId !== focusedBeatControl.laneId
                        || input.dataset.beatField !== focusedBeatControl.field) continue;
                    input.focus({ preventScroll: true });
                    if (focusedBeatControl.selectionStart != null && focusedBeatControl.selectionEnd != null) {
                        try {
                            input.setSelectionRange(focusedBeatControl.selectionStart, focusedBeatControl.selectionEnd);
                        } catch (_) {
                            // Number inputs do not support text selection in
                            // every webview implementation.
                        }
                    }
                    break;
                }
            }
        }

        function renderSelectedConnections() {
            if (!state.selectedLaneId) return;
            const selectedRow = laneWindow.querySelector('.lane-row[data-lane-id="' + CSS.escape(state.selectedLaneId) + '"]');
            if (!selectedRow) return;
            const overlay = document.createElementNS("http://www.w3.org/2000/svg", "svg");
            overlay.setAttribute("class", "connection-overlay");
            overlay.setAttribute("width", "100%");
            overlay.setAttribute("height", "100%");
            const windowRect = laneWindow.getBoundingClientRect();
            const visibleConnections = (state.connections || []).filter((connection) =>
                connection.sourceLaneId === state.selectedLaneId
                || connection.targetLaneId === state.selectedLaneId);
            // Incoming routes are deliberately drawn first: they remain
            // available as orientation context without competing with the
            // selected lane's outgoing routes.
            visibleConnections.sort((left, right) => {
                const leftIncoming = left.targetLaneId === state.selectedLaneId;
                const rightIncoming = right.targetLaneId === state.selectedLaneId;
                return Number(leftIncoming) - Number(rightIncoming);
            });
            for (const connection of visibleConnections) {
                const sourceRow = laneWindow.querySelector('.lane-row[data-lane-id="' + CSS.escape(String(connection.sourceLaneId)) + '"]');
                const targetRow = laneWindow.querySelector('.lane-row[data-lane-id="' + CSS.escape(String(connection.targetLaneId)) + '"]');
                if (!sourceRow || !targetRow) continue;
                const sourcePort = sourceRow.querySelector(".lane-connection-button");
                const targetPort = targetRow.querySelector(".lane-connection-button");
                if (!sourcePort) continue;
                const source = sourcePort.getBoundingClientRect();
                if (!targetPort) continue;
                const target = targetPort.getBoundingClientRect();
                // Both lane controls live on the left edge.  Start just to
                // the right of the source control, then approach the target
                // from its right so cables never disappear beneath buttons.
                const x1 = source.right - windowRect.left + 2;
                const y1 = source.top + source.height / 2 - windowRect.top;
                const x2 = target.right - windowRect.left + 2;
                const y2 = target.top + target.height / 2 - windowRect.top;
                const curve = Math.max(32, Math.abs(x2 - x1) * .35);
                const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
                path.setAttribute("class", "selected-connection "
                    + (connection.targetLaneId === state.selectedLaneId
                        ? "incoming-connection" : "outgoing-connection"));
                path.setAttribute("d", "M " + x1 + " " + y1
                    + " C " + (x1 + curve) + " " + y1
                    + ", " + (x2 + curve) + " " + y2
                    + ", " + x2 + " " + y2);
                overlay.appendChild(path);
            }
            laneWindow.appendChild(overlay);
        }

        function renderConnections() {
            connectionToggle.textContent = "Connections (" + String(Array.isArray(state.connections) ? state.connections.length : 0) + ")";
            connectionList.textContent = "";
            connectionList.style.display = state.connectionsExpanded ? "block" : "none";

            if (state.connectionsExpanded && Array.isArray(state.connections) && state.connections.length > 0) {
                for (const connection of state.connections) {
                    const row = document.createElement("div");
                    row.className = "connection-row " + connection.state;
                    row.title = connection.kind;

                    const source = document.createElement("div");
                    source.className = "connection-label";
                    source.textContent = "#" + String(connection.sourceLaneId) + " " + connection.sourceLabel;
                    row.appendChild(source);

                    const arrow = document.createElement("div");
                    arrow.className = "connection-arrow";
                    arrow.textContent = "->";
                    row.appendChild(arrow);

                    const target = document.createElement("div");
                    target.className = "connection-label";
                    target.textContent = "#" + String(connection.targetLaneId) + " " + connection.targetLabel;
                    row.appendChild(target);

                    const connectionState = document.createElement("div");
                    connectionState.className = "connection-state";
                    connectionState.textContent = connection.state;
                    row.appendChild(connectionState);

                    if (connection.sourceLaneId === state.selectedLaneId || connection.targetLaneId === state.selectedLaneId) {
                        const disconnect = document.createElement("button");
                        disconnect.type = "button";
                        disconnect.className = "zoom-button";
                        disconnect.textContent = "×";
                        disconnect.title = "Disconnect";
                        disconnect.addEventListener("click", () => vscode.postMessage({
                            type: "disconnectLanes",
                            sourceLaneId: connection.sourceLaneId,
                            targetLaneId: connection.targetLaneId,
                            portDomain: connection.portDomain,
                            portKind: connection.portKind,
                            portOrdinal: connection.portOrdinal,
                        }));
                        row.appendChild(disconnect);
                    }

                    connectionList.appendChild(row);
                }
            }
        }

        function render() {
            ensureLayout();
            renderTimelineChrome();
            renderLanes();
            renderConnections();
            postTimelineViewport();

            requestAnimationFrame(() => {
                if (!viewport) {
                    return;
                }
                // Restore only the browser's initial saved position.  After
                // that, scrollTop belongs exclusively to the browser; server
                // results may change lane data and the spacer's maximum, but
                // can never move the scrollbar.
                if (!hasAppliedInitialScrollPosition) {
                    viewport.scrollTop = laneVerticalOffset(state.startIndex);
                    hasAppliedInitialScrollPosition = true;
                }
                if (lastPostedVisibleLaneCount < 0) {
                    scheduleViewportPost();
                }
            });
        }

        window.addEventListener("message", (event) => {
            const message = event.data || {};
            if (message.type === "setSelection") {
                const selection = message.selection || {};
                state.selectedLaneIds = new Set(Array.isArray(selection.laneIds)
                    ? selection.laneIds.filter((laneId) => typeof laneId === "string") : []);
                state.selectedLaneId = typeof selection.primaryLaneId === "string" ? selection.primaryLaneId : "";
                state.selectionAnchorLaneId = typeof selection.anchorLaneId === "string" ? selection.anchorLaneId : "";
                state.selectionAnchorViewId = typeof selection.anchorViewId === "string" ? selection.anchorViewId : "";
                persistLaneViewState();
                renderLanes();
                renderConnections();
                return;
            }
            if (message.type === "setState") {
                const suppliedLanes = Array.isArray(message.lanes) ? message.lanes : [];
                const nextLanes = applyLaneOrder(suppliedLanes);
                const nextTotalLaneCount = Number(message.totalLaneCount || 0);
                const structureChanged = laneStructureKey(state.lanes, state.totalLaneCount)
                    !== laneStructureKey(nextLanes, nextTotalLaneCount);
                const connectionsChanged = JSON.stringify(state.connections)
                    !== JSON.stringify(Array.isArray(message.connections) ? message.connections : []);
                if (typeof message.laneViewId === "string") state.laneViewId = message.laneViewId;
                if (message.selection && typeof message.selection === "object") {
                    state.selectedLaneIds = new Set(Array.isArray(message.selection.laneIds)
                        ? message.selection.laneIds.filter((laneId) => typeof laneId === "string") : []);
                    state.selectedLaneId = typeof message.selection.primaryLaneId === "string"
                        ? message.selection.primaryLaneId : "";
                    state.selectionAnchorLaneId = typeof message.selection.anchorLaneId === "string"
                        ? message.selection.anchorLaneId : "";
                    state.selectionAnchorViewId = typeof message.selection.anchorViewId === "string"
                        ? message.selection.anchorViewId : "";
                }
                if (typeof message.laneQuery === "string") state.laneQuery = message.laneQuery;
                if (message.laneQuerySchema === null
                    || (message.laneQuerySchema && typeof message.laneQuerySchema === "object")) {
                    const schemaChanged = state.laneQuerySchema?.revision !== message.laneQuerySchema?.revision;
                    state.laneQuerySchema = message.laneQuerySchema;
                    if (schemaChanged) {
                        clearLaneQuerySuggestions();
                        if (document.activeElement === laneQueryInput) scheduleLaneQueryCompletion();
                    }
                }
                if (!viewport) {
                    state.startIndex = Number(message.startIndex || 0);
                    state.visibleLaneCount = Number(message.visibleLaneCount || 0);
                }
                state.totalLaneCount = nextTotalLaneCount;
                state.lanes = nextLanes;
                if (viewport) {
                    state.startIndex = laneIndexAtVerticalOffset(viewport.scrollTop);
                    state.visibleLaneCount = visibleLaneCountAtVerticalOffset(viewport.scrollTop, viewport.clientHeight);
                }
                state.connections = Array.isArray(message.connections) ? message.connections : [];
                syncLaneQueryControl();
                state.contentByLaneId = message.contentByLaneId && typeof message.contentByLaneId === "object"
                    ? message.contentByLaneId : state.contentByLaneId;
                if (typeof message.playbackSampleIndex === "number") {
                    state.playbackSampleIndex = message.playbackSampleIndex;
                }
                vscode.setState({
                    laneViewId: state.laneViewId,
                    laneQuery: state.laneQuery,
                    selectedLaneId: state.selectedLaneId,
                    selectedLaneIds: selectedLaneIds(),
                    selectionAnchorLaneId: state.selectionAnchorLaneId,
                    startIndex: state.startIndex,
                    visibleLaneCount: state.visibleLaneCount,
                    connectionsExpanded: state.connectionsExpanded,
                    queryExpanded: state.queryExpanded,
                expandedLaneSettingsIds: [...state.expandedLaneSettingsIds],
                    laneOrder: state.laneOrder,
                samplesPerPixel: state.samplesPerPixel,
                panSamples: state.panSamples,
                compiledLaneHeight: state.compiledLaneHeight,
                });
                // A UI-originated state update commonly produces a
                // laneViewUpdated notification even though the lane shape did
                // not change. Its content is already represented locally, so
                // update only dynamic output instead of replacing controls.
                if (!structureChanged && viewport) {
                    if (connectionsChanged) {
                        // The connection controls and selected-lane cables
                        // are part of each row, not the removed bottom panel.
                        // Rebuild those rows immediately after an RPC result.
                        renderLanes();
                    } else {
                        updateDynamicContent();
                    }
                    return;
                }
                // Lane-view refreshes may be emitted while the beat node
                // recompiles. They are structural messages, but replacing the
                // input during its pointer-lock gesture makes the browser
                // release pointer lock before the button is released.
                if (document.__ivBeatControlDrag?.active || isEditingBeatControl()) {
                    renderDeferredUntilBeatDragEnd = true;
                    return;
                }
                render();
            } else if (message.type === "laneQueryCompletion") {
                if (Number(message.requestId || 0) !== laneQueryCompletionRequestId
                    || !laneQueryInput || !message.completion) return;
                if (typeof message.completion.schemaRevision !== "number"
                    || message.completion.schemaRevision !== state.laneQuerySchema?.revision) return;
                laneQueryCompletion = message.completion;
                activeLaneQuerySuggestion = 0;
                renderLaneQuerySuggestions();
            } else if (message.type === "setContent") {
                state.contentByLaneId = message.contentByLaneId && typeof message.contentByLaneId === "object"
                    ? message.contentByLaneId : state.contentByLaneId;
                const nextUiStates = message.uiStateByLaneId && typeof message.uiStateByLaneId === "object"
                    ? message.uiStateByLaneId : state.uiStateByLaneId;
                const captureUiStateChanged = state.lanes.some((lane) => {
                    if (lane.modelTypeId !== "iv.timeline.audio-file-capture") return false;
                    const laneId = String(lane.laneId);
                    return state.uiStateByLaneId[laneId]?.serializedState
                        !== nextUiStates[laneId]?.serializedState;
                });
                state.uiStateByLaneId = nextUiStates;
                if (typeof message.playbackSampleIndex === "number") {
                    state.playbackSampleIndex = message.playbackSampleIndex;
                }
                // Content arrives at the visualization publish rate. Replacing
                // the lane DOM during a pointer-locked control drag destroys
                // the active numeric field and makes the gesture unreliable.
                if (document.__ivBeatControlDrag?.active) {
                    renderDeferredUntilBeatDragEnd = true;
                    return;
                }
                if (captureUiStateChanged) {
                    renderLanes();
                    return;
                }
                updateDynamicContent();
            }
        });

        new ResizeObserver(() => {
            scheduleViewportPost();
            renderTimelineChrome();
            postTimelineViewport();
        }).observe(root);

        render();
    </script>
</body>
</html>`;
    }
}

// GraphInputLanes stores an instance's trailing numeric id when available;
// UUID instance ids use libstdc++'s std::hash<string> narrowed to int.
// Mirror that identity here so lane metadata can be joined with the instance
// list already supplied by the workspace RPC.
function moduleInstanceNumericId(instanceId) {
    const suffix = instanceId.lastIndexOf(":") >= 0 ? instanceId.slice(instanceId.lastIndexOf(":") + 1) : "";
    if (/^-?\d+$/.test(suffix)) {
        const numeric = Number(suffix);
        if (Number.isSafeInteger(numeric)) {
            return numeric;
        }
    }
    return Number(BigInt.asIntN(32, libstdcxxStringHash(instanceId)));
}

function libstdcxxStringHash(value) {
    const bytes = new TextEncoder().encode(value);
    const mask = (1n << 64n) - 1n;
    const multiplier = 0xc6a4a7935bd1e995n;
    let hash = (0xc70f6907n ^ (BigInt(bytes.length) * multiplier)) & mask;
    let offset = 0;

    while (offset + 8 <= bytes.length) {
        let word = 0n;
        for (let byte = 0; byte < 8; ++byte) {
            word |= BigInt(bytes[offset + byte]) << BigInt(byte * 8);
        }
        word = (word * multiplier) & mask;
        word ^= word >> 47n;
        word = (word * multiplier) & mask;
        hash ^= word;
        hash = (hash * multiplier) & mask;
        offset += 8;
    }

    switch (bytes.length - offset) {
    case 7: hash ^= BigInt(bytes[offset + 6]) << 48n;
    case 6: hash ^= BigInt(bytes[offset + 5]) << 40n;
    case 5: hash ^= BigInt(bytes[offset + 4]) << 32n;
    case 4: hash ^= BigInt(bytes[offset + 3]) << 24n;
    case 3: hash ^= BigInt(bytes[offset + 2]) << 16n;
    case 2: hash ^= BigInt(bytes[offset + 1]) << 8n;
    case 1:
        hash ^= BigInt(bytes[offset]);
        hash = (hash * multiplier) & mask;
    }

    hash ^= hash >> 47n;
    hash = (hash * multiplier) & mask;
    hash ^= hash >> 47n;
    return hash & mask;
}
