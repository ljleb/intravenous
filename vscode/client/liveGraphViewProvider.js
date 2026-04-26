const vscode = require("vscode");
const fs = require("fs");
const path = require("path");

const mergedNodeIconPath = path.join(__dirname, "media", "merged_node.svg");
const singleNodeIconPath = path.join(__dirname, "media", "single_node.svg");
const arrowRightIconPath = path.join(__dirname, "media", "arrow_right.svg");
const chevronRightSourceIconPath = path.join(__dirname, "media", "chevron_left.svg");
const chevronDownIconPath = path.join(__dirname, "media", "chevron_down.svg");
const portIconSizePx = 16;

class LiveGraphViewProvider {
    constructor(extensionUri) {
        this.extensionUri = extensionUri;
        this.view = null;
        this.nodes = [];
        this.controlHandler = null;
    }

    setNodes(nodes) {
        this.nodes = Array.isArray(nodes) ? nodes : [];
        this.postState();
    }

    pruneDeletedNodeState(_deletedNodeIds) {
    }

    setControlHandler(handler) {
        this.controlHandler = handler;
    }

    updateSampleInputValue(nodeId, ordinal, value, memberOrdinal = null) {
        for (const node of this.nodes) {
            if (node.id !== nodeId) {
                continue;
            }
            const updateInputs = (inputs, markOverride) => {
                if (!Array.isArray(inputs)) {
                    return;
                }
                for (const input of inputs) {
                    if (Number(input.ordinal) === Number(ordinal)) {
                        input.currentValue = value;
                        if (markOverride != null) {
                            input.hasConcreteOverride = markOverride;
                        }
                    }
                }
            };
            const inputs = memberOrdinal == null
                ? node.sampleInputs
                : ((node.members || []).find((member) => Number(member.ordinal) === Number(memberOrdinal)) || {}).sampleInputs;
            updateInputs(inputs, memberOrdinal == null ? null : true);
            if (memberOrdinal == null) {
                for (const member of node.members || []) {
                    const memberInputs = Array.isArray(member.sampleInputs) ? member.sampleInputs : [];
                    for (const input of memberInputs) {
                        if (Number(input.ordinal) === Number(ordinal) && !input.hasConcreteOverride) {
                            input.currentValue = value;
                        }
                    }
                }
            }
        }
        this.postState();
    }

    clearSampleInputValueOverride(nodeId, memberOrdinal, ordinal) {
        for (const node of this.nodes) {
            if (node.id !== nodeId) {
                continue;
            }
            const logicalInput = Array.isArray(node.sampleInputs)
                ? node.sampleInputs.find((input) => Number(input.ordinal) === Number(ordinal))
                : null;
            const logicalValue = logicalInput && typeof logicalInput.currentValue === "number"
                ? logicalInput.currentValue
                : null;
            const member = (node.members || []).find((candidate) => Number(candidate.ordinal) === Number(memberOrdinal));
            const inputs = member && Array.isArray(member.sampleInputs) ? member.sampleInputs : [];
            for (const input of inputs) {
                if (Number(input.ordinal) === Number(ordinal)) {
                    input.hasConcreteOverride = false;
                    if (logicalValue != null) {
                        input.currentValue = logicalValue;
                    }
                }
            }
        }
        this.postState();
    }

    logicalIdentitySummary(node) {
        const identity = typeof node?.sourceIdentity === "string" && node.sourceIdentity.length > 0
            ? node.sourceIdentity
            : (typeof node?.id === "string" ? node.id : "");
        if (!identity) {
            return "";
        }

        const matches = [...identity.matchAll(/@([^@#:$]+)$/g)];
        if (matches.length > 0 && matches[0][1]) {
            return matches[0][1];
        }

        const atIndex = identity.lastIndexOf("@");
        if (atIndex >= 0 && atIndex + 1 < identity.length) {
            return identity.slice(atIndex + 1);
        }

        return identity;
    }

    sampleInputsForMemberView(node, member) {
        const logicalInputs = new Map();
        for (const input of node.sampleInputs || []) {
            logicalInputs.set(Number(input.ordinal), input);
        }
        return (member.sampleInputs || []).map((input) => {
            if (input.hasConcreteOverride) {
                return input;
            }
            const logicalInput = logicalInputs.get(Number(input.ordinal));
            if (!logicalInput || typeof logicalInput.currentValue !== "number") {
                return input;
            }
            return {
                ...input,
                currentValue: logicalInput.currentValue,
            };
        });
    }

    serializeNodes() {
        return this.nodes.map((node) => ({
            id: node.id || "",
            kind: node.kind || node.id || "",
            description: node.memberCount > 1
                ? (() => {
                    const identity = this.logicalIdentitySummary(node);
                    return identity ? `${identity} • ${node.memberCount} nodes` : `${node.memberCount} nodes`;
                })()
                : (this.logicalIdentitySummary(node) || node.id || ""),
            tooltip: `${node.kind || "node"}${this.logicalIdentitySummary(node) ? ` • ${this.logicalIdentitySummary(node)}` : ""}${node.memberCount > 1 ? ` • ${node.memberCount} members` : ""}`,
            memberCount: Number(node.memberCount || 0),
            icon: node.memberCount > 1 ? "merged" : "single",
            groups: [
                this.makePortGroup("sample inputs", node.sampleInputs || [], "input", "sample"),
                this.makePortGroup("sample outputs", node.sampleOutputs || [], "output", "sample"),
                this.makePortGroup("event inputs", node.eventInputs || [], "input", "event"),
                this.makePortGroup("event outputs", node.eventOutputs || [], "output", "event"),
            ].filter(Boolean),
            members: Array.isArray(node.members)
                ? node.members.map((member) => ({
                    ordinal: Number(member.ordinal || 0),
                    backingNodeId: member.backingNodeId || "",
                    kind: member.kind || node.kind || "",
                    description: `member ${Number(member.ordinal || 0)}`,
                    groups: [
                        this.makePortGroup("sample inputs", this.sampleInputsForMemberView(node, member), "input", "sample", true),
                        this.makePortGroup("sample outputs", member.sampleOutputs || [], "output", "sample", false),
                        this.makePortGroup("event inputs", member.eventInputs || [], "input", "event", false),
                        this.makePortGroup("event outputs", member.eventOutputs || [], "output", "event", false),
                    ].filter(Boolean),
                }))
                : [],
        }));
    }

    makePortGroup(label, ports, direction, portKind, forceTweakable = false) {
        if (!Array.isArray(ports) || ports.length === 0) {
            return null;
        }
        return {
            label,
            count: ports.length,
            direction,
            portKind,
            ports: ports.map((port, index) => ({
                name: port.name || `[${index}]`,
                connectivity: port.connectivity || "disconnected",
                ordinal: Number.isInteger(port.ordinal) ? port.ordinal : index,
                defaultValue: typeof port.defaultValue === "number" ? port.defaultValue : 0,
                currentValue: typeof port.currentValue === "number" ? port.currentValue : 0,
                hasConcreteOverride: Boolean(port.hasConcreteOverride),
                tweakable: (forceTweakable || direction === "input")
                    && direction === "input"
                    && portKind === "sample"
                    && (port.connectivity === "mixed" || port.connectivity === "disconnected"),
            })),
        };
    }

    resolveWebviewView(webviewView) {
        this.view = webviewView;
        webviewView.webview.options = {
            enableScripts: true,
            localResourceRoots: [vscode.Uri.joinPath(this.extensionUri, "media")],
        };
        webviewView.webview.onDidReceiveMessage(async (message) => {
            if (!message || !this.controlHandler) {
                return;
            }
            if (message.type === "setSampleInputValue" || message.type === "clearSampleInputValueOverride") {
                await this.controlHandler(message);
            }
        });
        webviewView.webview.html = this.getHtml(webviewView.webview);
        this.postState();
    }

    postState() {
        if (!this.view) {
            return;
        }
        this.view.webview.postMessage({
            type: "setState",
            nodes: this.serializeNodes(),
        });
    }

    getHtml(webview) {
        const nonce = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
        const mergedIconUri = webview.asWebviewUri(vscode.Uri.file(mergedNodeIconPath));
        const singleIconUri = webview.asWebviewUri(vscode.Uri.file(singleNodeIconPath));
        const arrowRightSvg = fs.readFileSync(arrowRightIconPath, "utf8")
            .replace(/fill=\"[^\"]*\"/g, 'fill="currentColor"');
        const chevronRightSvg = fs.readFileSync(chevronRightSourceIconPath, "utf8")
            .replace(/fill=\"[^\"]*\"/g, 'fill="currentColor"');
        const chevronDownSvg = fs.readFileSync(chevronDownIconPath, "utf8")
            .replace(/fill=\"[^\"]*\"/g, 'fill="currentColor"');

        return `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src ${webview.cspSource} data:; style-src ${webview.cspSource} 'unsafe-inline'; script-src 'nonce-${nonce}';">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <style>
        :root {
            color-scheme: light dark;
        }

        body {
            margin: 0;
            padding: 0;
            background: var(--vscode-sideBar-background);
            color: var(--vscode-foreground);
            font-family: var(--vscode-font-family);
            font-size: var(--vscode-font-size);
            font-weight: var(--vscode-font-weight);
            user-select: none;
            -webkit-user-select: none;
        }

        #root {
            display: flex;
            flex-direction: column;
            min-height: 100vh;
            user-select: none;
            -webkit-user-select: none;
        }

        .empty {
            padding: 8px 10px;
            color: var(--vscode-descriptionForeground);
            font-style: normal;
        }

        .tree-row {
            display: flex;
            align-items: center;
            gap: 6px;
            min-height: 22px;
            padding: 0 8px;
            box-sizing: border-box;
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
        }

        .tree-row:hover {
            background: var(--vscode-list-hoverBackground);
        }

        .tree-row:focus {
            outline: 1px solid var(--vscode-focusBorder);
            outline-offset: -1px;
        }

        body.knob-dragging .tree-row:hover {
            background: transparent;
        }

        .node {
            border: 0;
            border-bottom: 1px solid transparent;
        }

        .node-header {
            cursor: pointer;
            user-select: none;
        }

        .group-header {
            cursor: pointer;
            user-select: none;
        }

        .disclosure {
            width: 12px;
            flex: 0 0 12px;
            color: var(--vscode-descriptionForeground);
            display: inline-flex;
            align-items: center;
            justify-content: center;
        }

        .disclosure svg {
            display: block;
            width: 12px;
            height: 12px;
            fill: currentColor;
            pointer-events: none;
        }

        .disclosure svg.mirrored {
            transform: scaleX(-1);
            transform-origin: center;
        }

        .spacer {
            width: 10px;
            flex: 0 0 10px;
        }

        .node-icon {
            width: 16px;
            height: 16px;
            flex: 0 0 16px;
            opacity: 0.95;
            -webkit-user-drag: none;
        }

        .label {
            overflow: hidden;
            text-overflow: ellipsis;
            flex: 0 1 auto;
            user-select: none;
        }

        .description {
            flex: 0 1 auto;
            color: var(--vscode-descriptionForeground);
            overflow: hidden;
            text-overflow: ellipsis;
            user-select: none;
        }

        .node-children {
            display: block;
        }

        .node.collapsed > .node-children {
            display: none;
        }

        .group {
            display: block;
        }

        .group.collapsed > .group-ports {
            display: none;
        }

        .group-header {
            padding-left: 24px;
        }

        .member-header {
            padding-left: 42px;
        }

        .members-header {
            padding-left: 24px;
        }

        .member-group-header {
            padding-left: 60px;
        }

        .member-port-row {
            padding-left: 78px;
        }

        .port-row {
            padding-left: 42px;
        }

        .port-row.concrete-override .knob-band {
            stroke: var(--vscode-charts-orange);
        }

        .port-row.concrete-override .knob,
        .port-row.concrete-override .knob-value {
            opacity: 1;
        }

        .port-row.concrete-inherited .knob,
        .port-row.concrete-inherited .knob-value {
            opacity: 0.45;
        }

        .port-row.concrete-inherited .knob-band {
            opacity: 0.65;
        }

        .context-menu {
            position: fixed;
            z-index: 10;
            min-width: 168px;
            padding: 4px;
            border: 1px solid var(--vscode-menu-border, var(--vscode-widget-border));
            background: var(--vscode-menu-background, var(--vscode-dropdown-background));
            color: var(--vscode-menu-foreground, var(--vscode-foreground));
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.28);
        }

        .context-menu.hidden {
            display: none;
        }

        .context-menu button {
            display: block;
            width: 100%;
            border: 0;
            padding: 4px 8px;
            background: transparent;
            color: inherit;
            text-align: left;
            font: inherit;
            cursor: pointer;
        }

        .context-menu button:hover {
            background: var(--vscode-menu-selectionBackground, var(--vscode-list-hoverBackground));
        }

        .port-icon {
            width: ${portIconSizePx}px;
            flex: 0 0 ${portIconSizePx}px;
            color: var(--vscode-terminal-ansiWhite);
            text-align: center;
            opacity: 0.95;
            display: inline-flex;
            align-items: center;
            justify-content: center;
        }

        .port-icon svg {
            display: block;
            width: ${portIconSizePx}px;
            height: ${portIconSizePx}px;
            fill: currentColor;
            pointer-events: none;
        }

        .port-icon.mirrored svg {
            transform: scaleX(-1);
            transform-origin: center;
        }

        .port-row[data-direction="input"][data-connectivity="connected"] .port-icon,
        .port-row[data-direction="input"][data-connectivity="mixed"] .port-icon {
            color: var(--vscode-terminal-ansiYellow);
        }

        .port-row[data-direction="output"][data-connectivity="connected"] .port-icon,
        .port-row[data-direction="output"][data-connectivity="mixed"] .port-icon {
            color: var(--vscode-terminal-ansiBlue);
        }

        .port-connectivity {
            margin-left: auto;
            color: var(--vscode-descriptionForeground);
            text-transform: none;
        }

        .knob-wrap {
            margin-left: auto;
            display: inline-flex;
            align-items: center;
            gap: 6px;
            min-width: 64px;
        }

        .knob {
            width: 18px;
            height: 18px;
            display: inline-flex;
            align-items: center;
            justify-content: center;
            cursor: ns-resize;
            touch-action: none;
        }

        .knob svg {
            display: block;
            width: 18px;
            height: 18px;
            overflow: visible;
            pointer-events: none;
        }

        body.knob-dragging,
        body.knob-dragging * {
            cursor: none !important;
        }

        .knob-band {
            stroke: var(--vscode-terminal-ansiBlue);
            stroke-width: 1.8;
            stroke-linecap: round;
            fill: none;
        }

        .knob-value {
            min-width: 34px;
            text-align: right;
            color: var(--vscode-descriptionForeground);
            font-variant-numeric: tabular-nums;
            user-select: none;
        }

    </style>
</head>
<body>
    <div id="root"></div>
    <div id="contextMenu" class="context-menu hidden"></div>
    <script nonce="${nonce}">
        const vscode = acquireVsCodeApi();
        const root = document.getElementById("root");
        const contextMenu = document.getElementById("contextMenu");
        const state = {
            nodes: [],
            expanded: new Map(),
            pendingUpdates: new Map(),
            activeConcretePort: null,
            portRows: new Map(),
            renderedPortRows: new Set(),
        };
        const knobDrag = {
            active: null,
            currentValue: 0,
            lastClientX: 0,
            lastClientY: 0,
            pointerLockRequested: false,
        };
        let renderDeferredUntilDragEnd = false;
        const icons = {
            merged: ${JSON.stringify(String(mergedIconUri))},
            single: ${JSON.stringify(String(singleIconUri))},
            arrowRight: ${JSON.stringify(arrowRightSvg)},
            chevronRight: ${JSON.stringify(chevronRightSvg)},
            chevronDown: ${JSON.stringify(chevronDownSvg)},
        };

        function expandedValue(key, defaultValue) {
            return state.expanded.has(key) ? state.expanded.get(key) : defaultValue;
        }

        function toggleExpanded(key, defaultValue) {
            state.expanded.set(key, !expandedValue(key, defaultValue));
            render();
        }

        function row(label, description) {
            const element = document.createElement("div");
            element.className = "tree-row";

            const labelEl = document.createElement("div");
            labelEl.className = "label";
            labelEl.textContent = label;
            element.appendChild(labelEl);

            if (description) {
                const descriptionEl = document.createElement("div");
                descriptionEl.className = "description";
                descriptionEl.textContent = description;
                element.appendChild(descriptionEl);
            }

            return element;
        }

        function disclosure(isExpanded) {
            const el = document.createElement("div");
            el.className = "disclosure";
            el.innerHTML = isExpanded
                ? icons.chevronDown
                : icons.chevronRight.replace("<svg ", '<svg class="mirrored" ');
            return el;
        }

        function spacer() {
            const el = document.createElement("div");
            el.className = "spacer";
            return el;
        }

        function clamp(value, min, max) {
            return Math.min(max, Math.max(min, value));
        }

        function formatValue(value) {
            return Number(value).toFixed(3).replace(/\.?0+$/, (match) => match === ".000" ? "" : match);
        }

        function knobSvg(value) {
            const clamped = clamp(value, 0, 1);
            const circumference = 2 * Math.PI * 7;
            const visibleLength = circumference * 0.75 * clamped;
            const hiddenLength = Math.max(circumference - visibleLength, 0.001);
            return '<svg viewBox="0 0 18 18" aria-hidden="true">' +
                '<circle class="knob-band" cx="9" cy="9" r="7" ' +
                    'stroke-dasharray="' + visibleLength.toFixed(3) + ' ' + hiddenLength.toFixed(3) + '" ' +
                    'transform="rotate(135 9 9)"></circle>' +
                '</svg>';
        }

        function queueControlUpdate(nodeId, memberOrdinal, ordinal, value) {
            const key = String(nodeId) + ":" + String(memberOrdinal == null ? "" : memberOrdinal) + ":" + String(ordinal);
            const now = Date.now();
            const existing = state.pendingUpdates.get(key) || {
                lastSentAt: 0,
                lastSentValue: null,
                pendingValue: value,
                timeoutId: null,
            };
            existing.pendingValue = value;

            const sendNow = () => {
                if (existing.lastSentValue === existing.pendingValue) {
                    if (existing.timeoutId) {
                        clearTimeout(existing.timeoutId);
                        existing.timeoutId = null;
                    }
                    return;
                }
                existing.lastSentAt = Date.now();
                existing.lastSentValue = existing.pendingValue;
                if (existing.timeoutId) {
                    clearTimeout(existing.timeoutId);
                    existing.timeoutId = null;
                }
                vscode.postMessage({
                    type: "setSampleInputValue",
                    nodeId,
                    memberOrdinal,
                    inputOrdinal: ordinal,
                    value: existing.pendingValue,
                });
            };

            const elapsed = now - existing.lastSentAt;
            const intervalMs = 24;
            if (existing.lastSentValue === value && existing.timeoutId == null) {
                state.pendingUpdates.set(key, existing);
                return;
            }
            if (elapsed >= intervalMs) {
                sendNow();
                state.pendingUpdates.set(key, existing);
                return;
            }

            if (existing.timeoutId == null) {
                existing.timeoutId = setTimeout(() => {
                    sendNow();
                    state.pendingUpdates.set(key, existing);
                }, intervalMs - elapsed);
            }

            state.pendingUpdates.set(key, existing);
        }

        function clearPendingUpdate(nodeId, memberOrdinal, ordinal) {
            const key = String(nodeId) + ":" + String(memberOrdinal == null ? "" : memberOrdinal) + ":" + String(ordinal);
            const existing = state.pendingUpdates.get(key);
            if (existing && existing.timeoutId) {
                clearTimeout(existing.timeoutId);
            }
            state.pendingUpdates.delete(key);
        }

        function setActiveConcretePort(portRef) {
            state.activeConcretePort = portRef;
        }

        function hideContextMenu() {
            contextMenu.classList.add("hidden");
            contextMenu.textContent = "";
        }

        function clearSampleInputOverride(portRef) {
            if (!portRef || portRef.memberOrdinal == null || !portRef.hasConcreteOverride) {
                return;
            }
            clearPendingUpdate(portRef.nodeId, portRef.memberOrdinal, portRef.inputOrdinal);
            vscode.postMessage({
                type: "clearSampleInputValueOverride",
                nodeId: portRef.nodeId,
                memberOrdinal: portRef.memberOrdinal,
                inputOrdinal: portRef.inputOrdinal,
            });
            hideContextMenu();
        }

        function updatePortRowValue(portRow, value) {
            if (!portRow || !portRow.__knobDrag) {
                return;
            }
            const currentValue = clamp(Number(value || 0), 0, 1);
            portRow.__knobDrag.port.currentValue = currentValue;
            portRow.__knobDrag.knob.innerHTML = knobSvg(currentValue);
            portRow.__knobDrag.valueEl.textContent = formatValue(currentValue);
            if (knobDrag.active && knobDrag.active.knob === portRow.__knobDrag.knob) {
                knobDrag.currentValue = currentValue;
            }
        }

        function updateConcreteOverrideVisual(portRow, hasOverride) {
            if (!portRow || !portRow.__concretePortRef) {
                return;
            }
            portRow.__concretePortRef.hasConcreteOverride = hasOverride;
            if (portRow.__knobDrag && portRow.__knobDrag.port) {
                portRow.__knobDrag.port.hasConcreteOverride = hasOverride;
            }
            portRow.classList.toggle("concrete-override", hasOverride);
            portRow.classList.toggle("concrete-inherited", !hasOverride);
            const descriptionEl = portRow.querySelector(".description");
            if (descriptionEl) {
                descriptionEl.textContent = hasOverride ? "override" : "inherited";
            }
            if (portRow.__knobDrag && portRow.__knobDrag.port) {
                portRow.title = portRow.__knobDrag.port.name + " • " + (hasOverride ? "override" : "inherited from logical");
            }
        }

        function concretePortRefForRow(portRow) {
            if (!portRow || !portRow.__concretePortRef || !portRow.__knobDrag || !portRow.__knobDrag.port) {
                return null;
            }
            portRow.__concretePortRef.nodeId = portRow.__knobDrag.nodeId;
            portRow.__concretePortRef.memberOrdinal = portRow.__knobDrag.memberOrdinal;
            portRow.__concretePortRef.inputOrdinal = portRow.__knobDrag.port.ordinal;
            portRow.__concretePortRef.hasConcreteOverride = Boolean(portRow.__knobDrag.port.hasConcreteOverride);
            return portRow.__concretePortRef;
        }

        function applyLocalControlValue(nodeId, memberOrdinal, ordinal, value) {
            const currentValue = clamp(Number(value || 0), 0, 1);
            for (const node of state.nodes) {
                if (node.id !== nodeId) {
                    continue;
                }
                const updateInputs = (inputs, markOverride) => {
                    if (!Array.isArray(inputs)) {
                        return;
                    }
                    for (const input of inputs) {
                        if (Number(input.ordinal) === Number(ordinal)) {
                            input.currentValue = currentValue;
                            if (markOverride != null) {
                                input.hasConcreteOverride = markOverride;
                            }
                        }
                    }
                };
                if (memberOrdinal == null) {
                    updateInputs(node.sampleInputs, null);
                    for (const member of node.members || []) {
                        const memberInputs = Array.isArray(member.sampleInputs) ? member.sampleInputs : [];
                        for (const input of memberInputs) {
                            if (Number(input.ordinal) === Number(ordinal) && !input.hasConcreteOverride) {
                                input.currentValue = currentValue;
                            }
                        }
                    }
                } else {
                    const member = (node.members || []).find((candidate) => Number(candidate.ordinal) === Number(memberOrdinal));
                    updateInputs(member && member.sampleInputs, true);
                }
            }

            for (const portRow of state.portRows.values()) {
                if (!portRow.__knobDrag || portRow.__knobDrag.nodeId !== nodeId) {
                    continue;
                }
                const port = portRow.__knobDrag.port;
                if (!port || Number(port.ordinal) !== Number(ordinal)) {
                    continue;
                }
                if (memberOrdinal == null) {
                    if (portRow.__knobDrag.memberOrdinal == null) {
                        updatePortRowValue(portRow, currentValue);
                        continue;
                    }
                    if (!port.hasConcreteOverride) {
                        updatePortRowValue(portRow, currentValue);
                        updateConcreteOverrideVisual(portRow, false);
                    }
                    continue;
                }
                if (
                    portRow.__knobDrag.memberOrdinal != null
                    && Number(portRow.__knobDrag.memberOrdinal) === Number(memberOrdinal)
                ) {
                    updatePortRowValue(portRow, currentValue);
                    updateConcreteOverrideVisual(portRow, true);
                }
            }
        }

        function showContextMenu(event, portRef) {
            if (!portRef || !portRef.hasConcreteOverride) {
                hideContextMenu();
                return;
            }
            setActiveConcretePort(portRef);
            contextMenu.textContent = "";

            const clearButton = document.createElement("button");
            clearButton.type = "button";
            clearButton.textContent = "Follow logical value";
            clearButton.addEventListener("click", () => clearSampleInputOverride(portRef));
            contextMenu.appendChild(clearButton);

            contextMenu.classList.remove("hidden");
            const maxLeft = Math.max(0, window.innerWidth - contextMenu.offsetWidth - 4);
            const maxTop = Math.max(0, window.innerHeight - contextMenu.offsetHeight - 4);
            contextMenu.style.left = String(Math.min(event.clientX, maxLeft)) + "px";
            contextMenu.style.top = String(Math.min(event.clientY, maxTop)) + "px";
        }

        function resolvePortRowFromPoint(clientX, clientY) {
            const hit = document.elementFromPoint(clientX, clientY);
            if (!(hit instanceof Element)) {
                return null;
            }
            const portRow = hit.closest(".port-row");
            if (!portRow || !root.contains(portRow) || !portRow.__knobDrag) {
                return null;
            }
            return portRow;
        }

        function beginKnobDrag(lockEl, knob, valueEl, nodeId, memberOrdinal, port, event) {
            if (event.button !== 0) {
                return;
            }
            event.preventDefault();
            knobDrag.active = { knob, valueEl, nodeId, memberOrdinal, port };
            knobDrag.currentValue = clamp(Number(port.currentValue || 0), 0, 1);
            knobDrag.lastClientX = event.clientX;
            knobDrag.lastClientY = event.clientY;
            knobDrag.pointerLockRequested = false;
            document.body.classList.add("knob-dragging");
            try {
                if (typeof lockEl.requestPointerLock === "function") {
                    lockEl.requestPointerLock();
                    knobDrag.pointerLockRequested = true;
                } else if (typeof lockEl.setPointerCapture === "function" && event.pointerId != null) {
                    lockEl.setPointerCapture(event.pointerId);
                }
            } catch (_) {
            }
        }

        function endKnobDrag() {
            if (!knobDrag.active) {
                return;
            }
            knobDrag.active = null;
            document.body.classList.remove("knob-dragging");
            if (document.pointerLockElement) {
                try {
                    document.exitPointerLock();
                } catch (_) {
                }
            }
            if (renderDeferredUntilDragEnd) {
                renderDeferredUntilDragEnd = false;
                render();
            }
        }

        function applyDraggedKnobDelta(delta) {
            if (!knobDrag.active || delta === 0) {
                return;
            }
            knobDrag.currentValue = clamp(knobDrag.currentValue + delta, 0, 1);
            const { nodeId, memberOrdinal, port } = knobDrag.active;
            applyLocalControlValue(nodeId, memberOrdinal, port.ordinal, knobDrag.currentValue);
            queueControlUpdate(nodeId, memberOrdinal, port.ordinal, knobDrag.currentValue);
        }

        function applyDraggedKnobEvent(event) {
            if (!knobDrag.active) {
                return;
            }
            const usePointerLockMovement = document.pointerLockElement != null;
            const deltaX = usePointerLockMovement
                ? event.movementX
                : event.clientX - knobDrag.lastClientX;
            const deltaY = usePointerLockMovement
                ? event.movementY
                : event.clientY - knobDrag.lastClientY;
            if (!usePointerLockMovement) {
                knobDrag.lastClientX = event.clientX;
                knobDrag.lastClientY = event.clientY;
            }
            applyDraggedKnobDelta((deltaX - deltaY) / 180);
        }

        document.addEventListener("pointermove", (event) => {
            applyDraggedKnobEvent(event);
        });

        document.addEventListener("mousemove", (event) => {
            if (window.PointerEvent && document.pointerLockElement == null) {
                return;
            }
            applyDraggedKnobEvent(event);
        });

        document.addEventListener("mouseup", () => {
            endKnobDrag();
        });

        document.addEventListener("pointerup", () => {
            endKnobDrag();
        });

        document.addEventListener("pointercancel", () => {
            endKnobDrag();
        });

        document.addEventListener("pointerlockchange", () => {
            if (knobDrag.active && knobDrag.pointerLockRequested && document.pointerLockElement == null) {
                endKnobDrag();
            }
        });

        document.addEventListener("dragstart", (event) => {
            event.preventDefault();
        });

        document.addEventListener("pointerdown", (event) => {
            if (event.button !== 0 || knobDrag.active) {
                return;
            }
            if (!(event.target instanceof Element)) {
                return;
            }
            const resolved = resolvePortRowFromPoint(event.clientX, event.clientY);
            if (!resolved) {
                return;
            }
            const portRow = resolved;
            const { knob, valueEl, nodeId, memberOrdinal, port } = portRow.__knobDrag;
            if (portRow.__concretePortRef) {
                setActiveConcretePort(portRow.__concretePortRef);
            }
            beginKnobDrag(portRow, knob, valueEl, nodeId, memberOrdinal, port, event);
        }, true);

        function attachKnobBehavior(knob, valueEl, nodeId, memberOrdinal, port) {
            let currentValue = clamp(Number(port.currentValue || 0), 0, 1);

            const applyValue = (nextValue) => {
                currentValue = clamp(nextValue, 0, 1);
                applyLocalControlValue(nodeId, memberOrdinal, port.ordinal, currentValue);
                queueControlUpdate(nodeId, memberOrdinal, port.ordinal, currentValue);
                if (knobDrag.active && knobDrag.active.knob === knob) {
                    knobDrag.currentValue = currentValue;
                }
            };

            knob.innerHTML = knobSvg(currentValue);
            valueEl.textContent = formatValue(currentValue);

            knob.addEventListener("wheel", (event) => {
                event.preventDefault();
                const step = event.deltaY < 0 ? 0.02 : -0.02;
                applyValue(currentValue + step);
            }, { passive: false });

            knob.addEventListener("dblclick", (event) => {
                event.preventDefault();
                applyValue(clamp(Number(port.defaultValue || 0), 0, 1));
            });
        }

        function renderPort(parent, nodeId, memberOrdinal, group, port, index) {
            const memberKeyPart = memberOrdinal == null ? "" : \`/member:\${memberOrdinal}\`;
            const portKey = \`node:\${nodeId}\${memberKeyPart}/group:\${group.label}/port:\${index}\`;
            const portDescription = memberOrdinal != null && port.tweakable
                ? (port.hasConcreteOverride ? "override" : "inherited")
                : port.connectivity;
            state.renderedPortRows.add(portKey);

            let portRow = state.portRows.get(portKey);
            if (!portRow) {
                portRow = row(port.name, portDescription);
                portRow.classList.add("port-row");
                portRow.prepend(spacer());

                const icon = document.createElement("div");
                portRow.__portIcon = icon;
                portRow.insertBefore(icon, portRow.children[1]);
                state.portRows.set(portKey, portRow);
            }

            const labelEl = portRow.querySelector(".label");
            if (labelEl) {
                labelEl.textContent = port.name;
            }
            let descriptionEl = portRow.querySelector(".description");
            if (!descriptionEl) {
                descriptionEl = document.createElement("div");
                descriptionEl.className = "description";
                portRow.appendChild(descriptionEl);
            }
            descriptionEl.textContent = portDescription || "";

            portRow.className = "tree-row port-row";
            if (memberOrdinal != null) {
                portRow.classList.add("member-port-row");
                if (port.tweakable) {
                    portRow.classList.add(port.hasConcreteOverride ? "concrete-override" : "concrete-inherited");
                }
            }
            if (port.tweakable) {
                portRow.classList.add("has-control");
            }
            portRow.dataset.direction = group.direction;
            portRow.dataset.connectivity = port.connectivity;
            const icon = portRow.__portIcon;
            icon.className = group.direction === "input" ? "port-icon" : "port-icon mirrored";
            icon.innerHTML = icons.arrowRight;

            if (port.tweakable) {
                let knobWrap = portRow.__knobWrap;
                if (!knobWrap) {
                    knobWrap = document.createElement("div");
                    knobWrap.className = "knob-wrap";

                    const knob = document.createElement("div");
                    knob.className = "knob";
                    knob.title = "Drag vertically or use the mouse wheel to adjust. Double-click to reset.";

                    const valueEl = document.createElement("div");
                    valueEl.className = "knob-value";

                    knobWrap.appendChild(knob);
                    knobWrap.appendChild(valueEl);
                    portRow.appendChild(knobWrap);
                    portRow.__knobWrap = knobWrap;
                    portRow.__knobDrag = { knob, valueEl, nodeId, memberOrdinal, port };
                    attachKnobBehavior(knob, valueEl, nodeId, memberOrdinal, port);
                } else {
                    portRow.__knobDrag.nodeId = nodeId;
                    portRow.__knobDrag.memberOrdinal = memberOrdinal;
                    portRow.__knobDrag.port = port;
                    const currentValue = clamp(Number(port.currentValue || 0), 0, 1);
                    portRow.__knobDrag.knob.innerHTML = knobSvg(currentValue);
                    portRow.__knobDrag.valueEl.textContent = formatValue(currentValue);
                }
            } else if (portRow.__knobWrap) {
                portRow.__knobWrap.remove();
                portRow.__knobWrap = null;
                portRow.__knobDrag = null;
            }

            if (memberOrdinal != null && port.tweakable) {
                portRow.tabIndex = 0;
                if (!portRow.__concreteListenersAttached) {
                    portRow.__concretePortRef = {};
                    portRow.addEventListener("click", () => setActiveConcretePort(concretePortRefForRow(portRow)));
                    portRow.addEventListener("focus", () => setActiveConcretePort(concretePortRefForRow(portRow)));
                    portRow.addEventListener("contextmenu", (event) => {
                        event.preventDefault();
                        showContextMenu(event, concretePortRefForRow(portRow));
                    });
                    portRow.__concreteListenersAttached = true;
                }
                if (!portRow.__concretePortRef) {
                    portRow.__concretePortRef = {};
                }
                portRow.__concretePortRef.nodeId = nodeId;
                portRow.__concretePortRef.memberOrdinal = memberOrdinal;
                portRow.__concretePortRef.inputOrdinal = port.ordinal;
                portRow.__concretePortRef.hasConcreteOverride = Boolean(port.hasConcreteOverride);
            } else {
                portRow.removeAttribute("tabindex");
                portRow.__concretePortRef = null;
            }

            portRow.title = memberOrdinal != null && port.tweakable
                ? \`\${port.name} • \${port.hasConcreteOverride ? "override" : "inherited from logical"}\`
                : \`\${port.name} • \${port.connectivity}\`;
            parent.appendChild(portRow);
            return portKey;
        }

        function renderGroup(parent, nodeId, memberOrdinal, group) {
            const memberKeyPart = memberOrdinal == null ? "" : \`/member:\${memberOrdinal}\`;
            const groupKey = \`node:\${nodeId}\${memberKeyPart}/group:\${group.label}\`;
            const isExpanded = expandedValue(groupKey, true);

            const groupEl = document.createElement("div");
            groupEl.className = isExpanded ? "group" : "group collapsed";

            const header = row(group.label, String(group.count));
            header.classList.add("group-header");
            if (memberOrdinal != null) {
                header.classList.add("member-group-header");
            }
            header.prepend(disclosure(isExpanded));
            header.addEventListener("click", () => toggleExpanded(groupKey, true));
            groupEl.appendChild(header);

            const ports = document.createElement("div");
            ports.className = "group-ports";
            for (let i = 0; i < group.ports.length; ++i) {
                renderPort(ports, nodeId, memberOrdinal, group, group.ports[i], i);
            }
            groupEl.appendChild(ports);
            parent.appendChild(groupEl);
        }

        function renderMember(parent, node, member) {
            const memberKey = \`node:\${node.id}/member:\${member.ordinal}\`;
            const isExpanded = expandedValue(memberKey, false);

            const memberEl = document.createElement("div");
            memberEl.className = isExpanded ? "group" : "group collapsed";

            const header = row(member.kind || "member", member.description || \`member \${member.ordinal}\`);
            header.classList.add("member-header");
            header.prepend(disclosure(isExpanded));
            header.addEventListener("click", () => toggleExpanded(memberKey, false));
            memberEl.appendChild(header);

            const groupsEl = document.createElement("div");
            groupsEl.className = "group-ports";
            for (const group of member.groups) {
                renderGroup(groupsEl, node.id, member.ordinal, group);
            }
            memberEl.appendChild(groupsEl);
            parent.appendChild(memberEl);
        }

        function renderMembersGroup(parent, node) {
            if (!Array.isArray(node.members) || node.members.length === 0) {
                return;
            }
            const membersKey = \`node:\${node.id}/concrete-members\`;
            const isExpanded = expandedValue(membersKey, false);

            const groupEl = document.createElement("div");
            groupEl.className = isExpanded ? "group" : "group collapsed";

            const header = row("concrete members", String(node.members.length));
            header.classList.add("members-header");
            header.prepend(disclosure(isExpanded));
            header.addEventListener("click", () => toggleExpanded(membersKey, false));
            groupEl.appendChild(header);

            const membersEl = document.createElement("div");
            membersEl.className = "group-ports";
            for (const member of node.members) {
                renderMember(membersEl, node, member);
            }
            groupEl.appendChild(membersEl);
            parent.appendChild(groupEl);
        }

        function renderNode(parent, node) {
            const nodeKey = \`node:\${node.id}\`;
            const isExpanded = expandedValue(nodeKey, true);

            const nodeEl = document.createElement("div");
            nodeEl.className = isExpanded ? "node" : "node collapsed";

            const header = row(node.kind, node.description);
            header.classList.add("node-header");
            header.title = node.tooltip || node.kind;
            header.prepend(disclosure(isExpanded));

            const icon = document.createElement("img");
            icon.className = "node-icon";
            icon.src = node.icon === "merged" ? icons.merged : icons.single;
            icon.alt = "";
            header.insertBefore(icon, header.children[1]);
            header.addEventListener("click", () => toggleExpanded(nodeKey, true));
            nodeEl.appendChild(header);

            const children = document.createElement("div");
            children.className = "node-children";
            for (const group of node.groups) {
                renderGroup(children, node.id, null, group);
            }
            renderMembersGroup(children, node);
            nodeEl.appendChild(children);
            parent.appendChild(nodeEl);
        }

        function render() {
            state.renderedPortRows.clear();
            root.textContent = "";
            if (!Array.isArray(state.nodes) || state.nodes.length === 0) {
                const empty = document.createElement("div");
                empty.className = "empty";
                empty.textContent = "[no nodes]";
                empty.title = "No visible logical nodes at the current selection";
                root.appendChild(empty);
                return;
            }

            for (const node of state.nodes) {
                renderNode(root, node);
            }
            for (const [key, portRow] of state.portRows) {
                if (!state.renderedPortRows.has(key)) {
                    portRow.remove();
                    state.portRows.delete(key);
                }
            }
        }

        window.addEventListener("message", (event) => {
            const message = event.data || {};
            if (message.type === "setState") {
                state.nodes = Array.isArray(message.nodes) ? message.nodes : [];
                if (knobDrag.active) {
                    renderDeferredUntilDragEnd = true;
                    return;
                }
                render();
            }
        });

        window.addEventListener("keydown", (event) => {
            if (event.key === "Escape") {
                hideContextMenu();
                return;
            }
            if (event.key.toLowerCase() === "d" && state.activeConcretePort) {
                event.preventDefault();
                clearSampleInputOverride(state.activeConcretePort);
            }
        });

        window.addEventListener("click", (event) => {
            if (!contextMenu.contains(event.target)) {
                hideContextMenu();
            }
        });

        render();
    </script>
</body>
</html>`;
    }
}

module.exports = { LiveGraphViewProvider };
