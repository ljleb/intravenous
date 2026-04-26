const vscode = require("vscode");

class LaneViewProvider {
    constructor() {
        this.panel = null;
        this.lanes = [];
        this.connections = [];
        this.executionEpoch = 0;
        this.closeHandler = null;
        this.viewportHandler = null;
        this.startIndex = 0;
        this.visibleLaneCount = 24;
        this.totalLaneCount = 0;
    }

    setLanes(result) {
        this.executionEpoch = Number(result?.executionEpoch || 0);
        this.startIndex = Number(result?.startIndex || 0);
        this.visibleLaneCount = Number(result?.visibleLaneCount ?? this.visibleLaneCount ?? 24);
        this.totalLaneCount = Number(result?.totalLaneCount || 0);
        this.lanes = Array.isArray(result?.lanes) ? result.lanes : [];
        this.connections = Array.isArray(result?.connections) ? result.connections : [];
        this.postState();
    }

    clear() {
        this.executionEpoch = 0;
        this.startIndex = 0;
        this.totalLaneCount = 0;
        this.lanes = [];
        this.connections = [];
        this.postState();
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

    restoreViewportState(state) {
        if (!state || typeof state !== "object") {
            return;
        }
        this.startIndex = Math.max(0, Number(state.startIndex || 0));
        this.visibleLaneCount = Math.max(1, Number(state.visibleLaneCount || this.visibleLaneCount || 24));
    }

    viewportState() {
        return {
            startIndex: this.startIndex,
            visibleLaneCount: this.visibleLaneCount,
        };
    }

    serializeLanes() {
        return this.lanes.map((lane) => {
            const target = lane.target || {};
            const member = target.memberOrdinal == null ? "logical" : `member ${target.memberOrdinal}`;
            return {
                laneId: Number(lane.laneId || 0),
                domain: lane.domain || "realtime",
                laneType: lane.laneType || "graphInput",
                status: lane.status || "active",
                lastTouched: Number(lane.lastTouched || 0),
                title: target.portName || `[${target.portOrdinal ?? "?"}]`,
                description: `${target.portKind || "port"} • ${member}`,
                nodeId: target.logicalNodeId || "",
                portKind: target.portKind || "",
                portOrdinal: Number(target.portOrdinal || 0),
                portType: target.portType || "",
                memberOrdinal: target.memberOrdinal,
            };
        });
    }

    serializeConnections() {
        const lanesById = new Map(this.serializeLanes().map((lane) => [lane.laneId, lane]));
        return this.connections.map((connection) => {
            const sourceLaneId = Number(connection.sourceLaneId || 0);
            const targetLaneId = Number(connection.targetLaneId || 0);
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
                kind: connection.kind || "connection",
                state: connection.state || "active",
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
            if (!message || message.type !== "viewportChanged") {
                return;
            }
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
            executionEpoch: this.executionEpoch,
            startIndex: this.startIndex,
            visibleLaneCount: this.visibleLaneCount,
            totalLaneCount: this.totalLaneCount,
            lanes: this.serializeLanes(),
            connections: this.serializeConnections(),
        });
    }

    getHtml() {
        const nonce = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
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

        .summary,
        .empty,
        .section-title,
        .lane-row,
        .connection-row {
            min-height: 22px;
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
            margin-top: 6px;
            text-transform: uppercase;
            font-size: 0.85em;
            letter-spacing: 0;
        }

        .lane-viewport {
            position: relative;
            flex: 1 1 auto;
            overflow: auto;
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

        .lane-row:hover,
        .connection-row:hover {
            background: var(--vscode-list-hoverBackground);
        }

        .lane-id {
            flex: 0 0 auto;
            min-width: 40px;
            color: var(--vscode-terminal-ansiBlue);
            font-variant-numeric: tabular-nums;
        }

        .title {
            overflow: hidden;
            text-overflow: ellipsis;
        }

        .description {
            color: var(--vscode-descriptionForeground);
            overflow: hidden;
            text-overflow: ellipsis;
        }

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
        const laneHeight = 24;
        const restoredState = vscode.getState() || {};
        const state = {
            executionEpoch: 0,
            startIndex: Number(restoredState.startIndex || 0),
            visibleLaneCount: Number(restoredState.visibleLaneCount || 0),
            totalLaneCount: 0,
            lanes: [],
            connections: [],
            connectionsExpanded: Boolean(restoredState.connectionsExpanded),
        };
        let viewport = null;
        let pendingViewportPost = 0;
        let lastPostedStartIndex = -1;
        let lastPostedVisibleLaneCount = -1;
        let summary = null;
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
            if (summary) {
                summary.textContent = state.executionEpoch
                    ? String(state.startIndex) + "-" + String(state.startIndex + state.lanes.length) + " / " + String(state.totalLaneCount) + " lanes • " + String(state.connections.length) + " links • epoch " + String(state.executionEpoch)
                    : "lanes";
            }
            vscode.setState({
                startIndex: state.startIndex,
                visibleLaneCount: state.visibleLaneCount,
                connectionsExpanded: state.connectionsExpanded,
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
            summary.className = "summary";
            root.appendChild(summary);

            const laneSection = document.createElement("div");
            laneSection.className = "section-title";
            laneSection.textContent = "Lanes";
            root.appendChild(laneSection);

            viewport = document.createElement("div");
            viewport.className = "lane-viewport";
            viewport.addEventListener("scroll", scheduleViewportPost);
            viewport.addEventListener("wheel", (event) => {
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
                });
                renderConnections();
            });
            connectionSection.appendChild(connectionToggle);

            connectionList = document.createElement("div");
            connectionList.className = "connection-list";
            connectionSection.appendChild(connectionList);
            root.appendChild(connectionSection);
        }

        function renderLanes() {
            laneWindow.textContent = "";
            spacer.style.height = String(Math.max(0, state.totalLaneCount) * laneHeight) + "px";
            laneWindow.style.transform = "translateY(" + String(state.startIndex * laneHeight) + "px)";

            for (const lane of state.lanes) {
                const row = document.createElement("div");
                row.className = "lane-row";
                row.style.height = String(laneHeight) + "px";
                row.title = lane.nodeId;

                const id = document.createElement("div");
                id.className = "lane-id";
                id.textContent = "#" + String(lane.laneId);
                row.appendChild(id);

                const title = document.createElement("div");
                title.className = "title";
                title.textContent = lane.title;
                row.appendChild(title);

                const description = document.createElement("div");
                description.className = "description";
                description.textContent = lane.description;
                row.appendChild(description);

                laneWindow.appendChild(row);
            }

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
            summary.textContent = state.executionEpoch
                ? String(state.startIndex) + "-" + String(state.startIndex + state.lanes.length) + " / " + String(state.totalLaneCount) + " lanes • " + String(state.connections.length) + " links • epoch " + String(state.executionEpoch)
                : "lanes";
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
                state.executionEpoch = Number(message.executionEpoch || 0);
                state.startIndex = Number(message.startIndex || 0);
                state.visibleLaneCount = Number(message.visibleLaneCount || 0);
                state.totalLaneCount = Number(message.totalLaneCount || 0);
                state.lanes = Array.isArray(message.lanes) ? message.lanes : [];
                state.connections = Array.isArray(message.connections) ? message.connections : [];
                vscode.setState({
                    startIndex: state.startIndex,
                    visibleLaneCount: state.visibleLaneCount,
                    connectionsExpanded: state.connectionsExpanded,
                });
                render();
            }
        });

        new ResizeObserver(scheduleViewportPost).observe(root);

        render();
    </script>
</body>
</html>`;
    }
}

module.exports = { LaneViewProvider };
