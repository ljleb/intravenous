// @ts-nocheck
import * as vscode from "vscode";
import { lanePresentationPlugins } from "./lanePlugins";

export class LaneViewProvider {
    constructor() {
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
        this.startIndex = 0;
        this.visibleLaneCount = 24;
        this.totalLaneCount = 0;
        this.instanceNamesByNumericId = new Map();
        this.laneViewId = "";
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

    setLanes(result) {
        this.startIndex = Number(result?.startIndex || 0);
        this.visibleLaneCount = Number(result?.visibleLaneCount ?? this.visibleLaneCount ?? 24);
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

    restoreViewportState(state) {
        if (!state || typeof state !== "object") {
            return;
        }
        this.startIndex = Math.max(0, Number(state.startIndex || 0));
        this.visibleLaneCount = Math.max(1, Number(state.visibleLaneCount || this.visibleLaneCount || 24));
        if (typeof state.laneViewId === "string") this.laneViewId = state.laneViewId;
    }

    laneViewId() { return this.laneViewId || null; }

    setLaneViewId(viewId) {
        this.laneViewId = viewId;
        this.postState();
    }

    viewportState() {
        return {
            startIndex: this.startIndex,
            visibleLaneCount: this.visibleLaneCount,
        };
    }

    serializeLanes() {
        return this.lanes.map((lane) => {
            const metadata = lane && typeof lane.metadata === "object" && lane.metadata ? lane.metadata : {};
            const has = (key) => Object.prototype.hasOwnProperty.call(metadata, key);
            const type = has("dsp_graph.event") ? "event" : has("dsp_graph.sample") ? "sample" : "lane";
            const direction = has("dsp_graph.graph_output") ? "out" : has("dsp_graph.graph_input") ? "in" : "";
            const instanceName = has("dsp_graph.public")
                && type === "sample"
                && direction === "out"
                ? this.instanceNamesByNumericId.get(Number(metadata["dsp_graph.module_instance_id"]))
                : "";
            const title = (has("dsp_graph.public") ? "public " : "") + type + (direction ? " " + direction : "")
                + (instanceName ? ` • ${instanceName}` : "");
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
            if (this.closeHandler) {
                this.closeHandler();
            }
        });
        this.panel.webview.onDidReceiveMessage((message) => {
            if (!message) {
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
            startIndex: this.startIndex,
            visibleLaneCount: this.visibleLaneCount,
            totalLaneCount: this.totalLaneCount,
            lanes: this.serializeLanes(),
            connections: this.serializeConnections(),
            contentByLaneId: this.contentByLaneId,
            uiStateByLaneId: this.uiStateByLaneId,
            playbackSampleIndex: this.playbackSampleIndex,
        });
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
            display: flex;
            align-items: stretch;
            border-bottom: 1px solid var(--vscode-sideBarSectionHeader-border, rgba(128,128,128,.16));
            box-sizing: border-box;
        }
        .lane-label {
            width: 164px;
            flex: 0 0 164px;
            min-width: 0;
            display: flex;
            align-items: center;
            gap: 6px;
            padding: 0 8px;
            box-sizing: border-box;
            background: var(--vscode-sideBar-background);
            border-right: 1px solid var(--vscode-sideBarSectionHeader-border, rgba(128,128,128,.18));
        }
        .lane-row.realtime .lane-label { display: none; }

        .lane-domain {
            flex: 0 0 auto;
            width: 8px;
            height: 8px;
            border-radius: 50%;
            background: var(--vscode-charts-blue);
        }

        .lane-domain.compiled {
            background: var(--vscode-charts-orange);
        }

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
        .lane-signal {
            position: absolute; left: 0; right: 0; top: 27px; height: 3px;
            background: var(--vscode-charts-blue);
            opacity: .55;
        }
        .lane-signal.compiled { background: var(--vscode-charts-orange); }
        .lane-signal.events { height: 10px; top: 23px; opacity: .8; background: repeating-linear-gradient(90deg, var(--vscode-charts-purple) 0 2px, transparent 2px 18px); }
        .playhead { position: absolute; z-index: 3; top: 0; bottom: 0; width: 2px; background: var(--vscode-editorCursor-foreground, var(--vscode-charts-orange)); box-shadow: 0 0 5px var(--vscode-editorCursor-foreground, var(--vscode-charts-orange)); pointer-events: none; }
        .canvas-playhead { position: absolute; z-index: 5; top: 0; bottom: 0; width: 2px; background: var(--vscode-editorCursor-foreground, var(--vscode-charts-orange)); box-shadow: 0 0 5px var(--vscode-editorCursor-foreground, var(--vscode-charts-orange)); pointer-events: none; }
        .realtime-face {
            flex: 1 1 auto; position: relative; display: flex; flex-direction: column; gap: 3px; padding: 5px 14px;
            background: var(--vscode-sideBar-background);
        }
        .realtime-meter-name { color: var(--vscode-foreground); font-size: .78em; font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
        .large-meter { position: relative; height: 12px; flex: 0 0 12px; overflow: hidden; border: 1px solid var(--vscode-editorWidget-border, rgba(128,128,128,.3)); background: var(--vscode-editor-background); }
        .large-meter-fill { position: absolute; top: 0; bottom: 0; left: 0; background: var(--vscode-charts-green); }
        .large-meter-grid { position: absolute; inset: 0; pointer-events: none; }
        .large-meter-tick { position: absolute; top: 0; bottom: 0; width: 1px; background: var(--vscode-editorWidget-border, rgba(128,128,128,.4)); }
        .large-meter-tick span { position: absolute; top: 2px; left: 3px; color: var(--vscode-descriptionForeground); font-size: .68em; white-space: nowrap; }
        .large-meter[data-channel]::after { content: attr(data-channel); position: absolute; z-index: 3; left: 3px; bottom: 1px; font-size: .62em; color: var(--vscode-descriptionForeground); }
        .meter-menu { position: fixed; z-index: 20; padding: 3px; min-width: 130px; background: var(--vscode-menu-background); border: 1px solid var(--vscode-menu-border, var(--vscode-editorWidget-border)); box-shadow: 0 2px 10px rgba(0,0,0,.35); }
        .meter-menu button { display: block; width: 100%; border: 0; padding: 5px 9px; text-align: left; color: var(--vscode-menu-foreground); background: transparent; cursor: pointer; }
        .meter-menu button:hover { background: var(--vscode-menu-selectionBackground); color: var(--vscode-menu-selectionForeground); }

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
        const restoredState = vscode.getState() || {};
        const state = {
            laneViewId: typeof restoredState.laneViewId === "string" ? restoredState.laneViewId : "",
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
        };
        const lanePresentationPlugins = new Map([${lanePluginRegistrations}].map((plugin) => [plugin.typeId, plugin]));
        let viewport = null;
        let pendingViewportPost = 0;
        let lastPostedStartIndex = -1;
        let lastPostedVisibleLaneCount = -1;
        let summary = null;
        let timelineRuler = null;
        let spacer = null;
        let laneWindow = null;
        let connectionToggle = null;
        let connectionList = null;

        function postViewportState() {
            if (!viewport) {
                return;
            }
            const nextStartIndex = Math.max(0, Math.floor(viewport.scrollTop / laneHeight));
            const nextVisibleLaneCount = Math.max(1, Math.ceil(viewport.clientHeight / laneHeight) + 1);
            state.startIndex = nextStartIndex;
            state.visibleLaneCount = nextVisibleLaneCount;
            if (laneWindow) {
                laneWindow.style.transform = "translateY(" + String(state.startIndex * laneHeight) + "px)";
            }
            renderTimelineChrome();
            vscode.setState({
                laneViewId: state.laneViewId,
                startIndex: state.startIndex,
                visibleLaneCount: state.visibleLaneCount,
                connectionsExpanded: state.connectionsExpanded,
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
            const currentStartIndex = Math.floor(viewport.scrollTop / laneHeight);
            const nextStartIndex = clampStartIndex(currentStartIndex + deltaRows);
            viewport.scrollTop = nextStartIndex * laneHeight;
            scheduleViewportPost();
        }

        function scheduleViewportPost(delayMs = 0) {
            if (pendingViewportPost) {
                return;
            }
            pendingViewportPost = requestAnimationFrame(() => {
                pendingViewportPost = 0;
                postViewportState();
            });
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

            timelineRuler = document.createElement("div");
            timelineRuler.className = "timeline-ruler";
            let panStartX = null;
            let panStartSamples = 0;
            let scrubPointerId = null;
            timelineRuler.addEventListener("pointerdown", (event) => {
                if (!event.shiftKey) {
                    scrubToSample(rulerStart() + (event.clientX - timelineRuler.getBoundingClientRect().left) * state.samplesPerPixel);
                    scrubPointerId = event.pointerId;
                    timelineRuler.setPointerCapture(event.pointerId);
                    return;
                }
                panStartX = event.clientX;
                panStartSamples = state.panSamples;
                timelineRuler.setPointerCapture(event.pointerId);
            });
            timelineRuler.addEventListener("pointermove", (event) => {
                if (scrubPointerId === event.pointerId) {
                    scrubToSample(rulerStart() + (event.clientX - timelineRuler.getBoundingClientRect().left) * state.samplesPerPixel);
                    return;
                }
                if (panStartX == null) return;
                state.panSamples = panStartSamples - (event.clientX - panStartX) * state.samplesPerPixel;
                renderTimelineChrome();
                renderLanes();
            });
            timelineRuler.addEventListener("pointerup", (event) => {
                if (scrubPointerId === event.pointerId) {
                    scrubPointerId = null;
                    timelineRuler.releasePointerCapture(event.pointerId);
                    return;
                }
                if (panStartX == null) return;
                panStartX = null;
                timelineRuler.releasePointerCapture(event.pointerId);
                vscode.setState({ ...vscode.getState(), panSamples: state.panSamples });
            });
            root.appendChild(timelineRuler);

            viewport = document.createElement("div");
            viewport.className = "lane-viewport";
            viewport.addEventListener("scroll", scheduleViewportPost);
            viewport.addEventListener("wheel", (event) => {
                if (event.altKey) {
                    event.preventDefault();
                    setVerticalZoom(state.compiledLaneHeight * (event.deltaY > 0 ? 0.85 : 1.18));
                    return;
                }
                if (event.ctrlKey || event.metaKey) {
                    event.preventDefault();
                    setZoom(state.samplesPerPixel * (event.deltaY > 0 ? 1.25 : 0.8));
                    return;
                }
                if (event.shiftKey) {
                    event.preventDefault();
                    state.panSamples += event.deltaY * state.samplesPerPixel;
                    renderTimelineChrome();
                    renderLanes();
                    return;
                }
                const rowDelta = Math.trunc(event.deltaY / laneHeight) || Math.sign(event.deltaY);
                if (rowDelta !== 0) {
                    event.preventDefault();
                    moveViewport(rowDelta);
                }
            }, { passive: false });

            spacer = document.createElement("div");
            spacer.className = "lane-spacer";
            spacer.style.height = String(Math.max(0, state.totalLaneCount) * laneHeight) + "px";
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
                    samplesPerPixel: state.samplesPerPixel,
                    panSamples: state.panSamples,
                });
                renderConnections();
            });
            connectionSection.appendChild(connectionToggle);

            connectionList = document.createElement("div");
            connectionList.className = "connection-list";
            connectionSection.appendChild(connectionList);
            root.appendChild(connectionSection);
        }

        function setZoom(samplesPerPixel) {
            state.samplesPerPixel = Math.max(1, Math.min(65536, Math.round(samplesPerPixel)));
            vscode.setState({
                startIndex: state.startIndex,
                visibleLaneCount: state.visibleLaneCount,
                connectionsExpanded: state.connectionsExpanded,
                samplesPerPixel: state.samplesPerPixel,
                panSamples: state.panSamples,
                compiledLaneHeight: state.compiledLaneHeight,
            });
            renderLanes();
            renderTimelineChrome();
        }

        function setVerticalZoom(height) {
            state.compiledLaneHeight = Math.max(34, Math.min(220, Math.round(height)));
            vscode.setState({
                startIndex: state.startIndex,
                visibleLaneCount: state.visibleLaneCount,
                connectionsExpanded: state.connectionsExpanded,
                samplesPerPixel: state.samplesPerPixel,
                panSamples: state.panSamples,
                compiledLaneHeight: state.compiledLaneHeight,
            });
            renderLanes();
        }

        function showMeterMenu(event) {
            event.preventDefault();
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
            menu.style.left = String(event.clientX) + "px";
            menu.style.top = String(event.clientY) + "px";
            document.body.appendChild(menu);
            setTimeout(() => document.addEventListener("pointerdown", () => menu.remove(), { once: true }), 0);
        }

        function timelineStart() {
            return Math.max(0, Math.round(state.panSamples));
        }

        function playheadX() {
            return (Number(state.playbackSampleIndex || 0) - timelineStart()) / state.samplesPerPixel;
        }

        function scrubToSample(sampleIndex) {
            sampleIndex = Math.max(0, Math.round(sampleIndex));
            state.playbackSampleIndex = sampleIndex;
            vscode.postMessage({ type: "scrubPlayback", sampleIndex });
            renderTimelineChrome();
            renderLanes();
        }

        function rulerStart() {
            return timelineStart() - 164 * state.samplesPerPixel;
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
            cursor.style.left = String(164 + playheadX()) + "px";
            cursor.title = "playback sample " + String(state.playbackSampleIndex ?? 0);
            timelineRuler.appendChild(cursor);
        }

        function renderLanes() {
            laneWindow.textContent = "";
            spacer.style.height = String(Math.max(0, state.totalLaneCount) * laneHeight) + "px";
            laneWindow.style.transform = "translateY(" + String(state.startIndex * laneHeight) + "px)";

            for (const lane of state.lanes) {
                const row = document.createElement("div");
                row.className = "lane-row " + lane.domain;
                row.style.height = String(lane.domain === "compiled" ? state.compiledLaneHeight : laneHeight) + "px";
                row.title = "lane " + String(lane.laneId)
                    + (lane.description ? " • " + lane.description : "");

                const label = document.createElement("div");
                label.className = "lane-label";

                const domain = document.createElement("div");
                domain.className = "lane-domain " + lane.domain;
                domain.title = lane.domain;
                label.appendChild(domain);

                const title = document.createElement("div");
                title.className = "title";
                title.textContent = lane.title;
                title.title = lane.description || String(lane.laneId);
                if (lane.domain === "compiled") {
                    label.appendChild(title);
                }

                const content = state.contentByLaneId[String(lane.laneId)];
                const eventCount = Number(content?.eventCount || 0);
                const peakLevel = content?.peakLevel;
                if (lane.domain === "compiled" && content) {
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
                if (lane.domain !== "compiled") {
                    const staticFace = document.createElement("div");
                    staticFace.className = "realtime-face";
                    staticFace.title = "Right-click to choose decibel or sample-value scale";
                    const meterName = document.createElement("div");
                    meterName.className = "realtime-meter-name";
                    meterName.textContent = lane.title;
                    staticFace.appendChild(meterName);
                    const meter = document.createElement("div");
                    meter.className = "large-meter";
                    const isStereo = content?.sampleChannelType === "stereo";
                    if (isStereo) meter.dataset.channel = "L";
                    const fill = document.createElement("div");
                    fill.className = "large-meter-fill";
                    const linearLevel = peakLevel != null
                        ? Math.max(0, Number(peakLevel))
                        : Math.min(1, eventCount / 8);
                    const meterMaximum = state.meterGrid === "decibel"
                        ? Math.pow(10, 4 / 20) : 1;
                    const halfLogPosition = (value) => {
                        const xMin = Math.pow(10, -60 / 20) / meterMaximum;
                        const s = Math.sqrt(xMin);
                        const normalized = Math.min(1, Math.max(xMin, value / meterMaximum));
                        return Math.log((normalized + s) / (s * (1 + s))) / Math.log(1 / s);
                    };
                    const meterPosition = state.meterGrid === "decibel"
                        ? halfLogPosition(linearLevel)
                        : Math.min(1, linearLevel);
                    fill.style.width = String(meterPosition * 100) + "%";
                    meter.appendChild(fill);
                    const grid = document.createElement("div");
                    grid.className = "large-meter-grid";
                    const ticks = state.meterGrid === "decibel"
                        ? Array.from({ length: 22 }, (_, index) => {
                            const db = -60 + index * 3;
                            return [halfLogPosition(Math.pow(10, db / 20)), db >= -30 && db % 6 === 0
                                ? (db > 0 ? "+" : "") + String(db) + " dB"
                                : ""];
                        })
                        : [[0, "0"], [.25, ".25"], [.5, ".5"], [.75, ".75"], [1, "1"]];
                    for (const [position, text] of ticks) {
                        const tick = document.createElement("div");
                        tick.className = "large-meter-tick";
                        tick.style.left = String(position * 100) + "%";
                        const labelText = document.createElement("span");
                        labelText.textContent = text;
                        tick.appendChild(labelText);
                        grid.appendChild(tick);
                    }
                    meter.appendChild(grid);
                    staticFace.appendChild(meter);
                    if (isStereo) {
                        const rightMeter = meter.cloneNode(true);
                        rightMeter.dataset.channel = "R";
                        const rightLevel = Math.max(0, Number(content?.secondaryPeakLevel || 0));
                        const rightPosition = state.meterGrid === "decibel"
                            ? halfLogPosition(rightLevel)
                            : Math.min(1, rightLevel);
                        rightMeter.querySelector(".large-meter-fill").style.width = String(rightPosition * 100) + "%";
                        staticFace.appendChild(rightMeter);
                    }
                    staticFace.addEventListener("contextmenu", (event) => showMeterMenu(event));
                    row.appendChild(staticFace);
                    laneWindow.appendChild(row);
                    continue;
                }
                const presentation = lanePresentationPlugins.get(lane.modelTypeId);
                if (presentation?.render({ lane, content, row, state, laneWindow, timelineStart, scrubToSample,
                    postLaneUiState: (laneId, serializedState, expectedRevision) => vscode.postMessage({
                        type: "setLaneUiState", laneId, serializedState, expectedRevision,
                    }),
                })) continue;
                const track = document.createElement("div");
                track.className = "lane-track";
                track.style.setProperty("--second-width", String(Math.max(1, 48000 / state.samplesPerPixel)) + "px");
                const signal = document.createElement("div");
                signal.className = "lane-signal " + lane.domain + (eventCount > 0 ? " events" : "");
                const level = peakLevel != null ? Math.max(0.08, Math.min(1, Number(peakLevel))) : eventCount > 0 ? 0.65 : 0.16;
                signal.style.opacity = String(0.18 + level * 0.8);
                track.appendChild(signal);
                row.appendChild(track);

                laneWindow.appendChild(row);
            }

            const canvasPlayhead = document.createElement("div");
            canvasPlayhead.className = "canvas-playhead";
            canvasPlayhead.style.left = String(164 + playheadX()) + "px";
            canvasPlayhead.title = "playback sample " + String(state.playbackSampleIndex ?? 0);
            laneWindow.appendChild(canvasPlayhead);

            if (!Array.isArray(state.lanes) || state.lanes.length === 0) {
                const empty = document.createElement("div");
                empty.className = "empty";
                empty.textContent = state.totalLaneCount > 0 ? "[scrolling]" : "[no lanes]";
                laneWindow.appendChild(empty);
            }
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

                    connectionList.appendChild(row);
                }
            }
        }

        function render() {
            ensureLayout();
            renderTimelineChrome();
            renderLanes();
            renderConnections();

            requestAnimationFrame(() => {
                if (!viewport) {
                    return;
                }
                const desiredScrollTop = state.startIndex * laneHeight;
                if (Math.abs(viewport.scrollTop - desiredScrollTop) >= 1) {
                    viewport.scrollTop = desiredScrollTop;
                }
                if (lastPostedVisibleLaneCount < 0) {
                    scheduleViewportPost();
                }
            });
        }

        window.addEventListener("message", (event) => {
            const message = event.data || {};
            if (message.type === "setState") {
                if (typeof message.laneViewId === "string") state.laneViewId = message.laneViewId;
                state.startIndex = Number(message.startIndex || 0);
                state.visibleLaneCount = Number(message.visibleLaneCount || 0);
                state.totalLaneCount = Number(message.totalLaneCount || 0);
                state.lanes = Array.isArray(message.lanes) ? message.lanes : [];
                state.connections = Array.isArray(message.connections) ? message.connections : [];
                state.contentByLaneId = message.contentByLaneId && typeof message.contentByLaneId === "object"
                    ? message.contentByLaneId : state.contentByLaneId;
                if (typeof message.playbackSampleIndex === "number") {
                    state.playbackSampleIndex = message.playbackSampleIndex;
                }
                vscode.setState({
                    laneViewId: state.laneViewId,
                    startIndex: state.startIndex,
                    visibleLaneCount: state.visibleLaneCount,
                    connectionsExpanded: state.connectionsExpanded,
                samplesPerPixel: state.samplesPerPixel,
                panSamples: state.panSamples,
                compiledLaneHeight: state.compiledLaneHeight,
                });
                render();
            } else if (message.type === "setContent") {
                state.contentByLaneId = message.contentByLaneId && typeof message.contentByLaneId === "object"
                    ? message.contentByLaneId : state.contentByLaneId;
                state.uiStateByLaneId = message.uiStateByLaneId && typeof message.uiStateByLaneId === "object"
                    ? message.uiStateByLaneId : state.uiStateByLaneId;
                if (typeof message.playbackSampleIndex === "number") {
                    state.playbackSampleIndex = message.playbackSampleIndex;
                }
                renderTimelineChrome();
                renderLanes();
            }
        });

        new ResizeObserver(scheduleViewportPost).observe(root);

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
