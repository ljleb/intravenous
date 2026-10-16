import * as vscode from "vscode";
import * as fs from "fs";
import * as path from "path";

import { VirtualNode } from "./graphModel";
import {
    LiveGraphInstance,
    serializeLiveGraphInstances,
    serializeLiveGraphNodes,
} from "./liveGraphModel";
import {
    isLiveGraphControlMessage,
    LiveGraphControlHandler,
    LiveGraphSetInstancesMessage,
    LiveGraphSetPackageRootMessage,
    LiveGraphSetNodesMessage,
    LiveGraphSetSelectedInstanceMessage,
    LiveGraphUpsertNodesMessage,
} from "./liveGraphProtocol";

const mergedNodeIconPath = path.join(__dirname, "media", "merged_node.svg");
const singleNodeIconPath = path.join(__dirname, "media", "single_node.svg");
const arrowRightIconPath = path.join(__dirname, "media", "arrow_right.svg");
const chevronRightSourceIconPath = path.join(__dirname, "media", "chevron_left.svg");
const chevronDownIconPath = path.join(__dirname, "media", "chevron_down.svg");
const portIconSizePx = 16;

export class LiveGraphViewProvider {
    private readonly extensionUri: vscode.Uri;
    private view: vscode.WebviewView | null = null;
    private instances: LiveGraphInstance[] = [];
    private nodes: VirtualNode[] = [];
    private selectedInstanceId: string | null = null;
    private packageRoot: string | null = null;
    private controlHandler: LiveGraphControlHandler | null = null;

    constructor(extensionUri: vscode.Uri) {
        this.extensionUri = extensionUri;
    }

    setNodes(nodes: VirtualNode[]): void {
        this.nodes = Array.isArray(nodes) ? nodes : [];
        this.postNodes();
    }

    setInstances(instances: LiveGraphInstance[]): void {
        this.instances = Array.isArray(instances) ? instances : [];
        const validInstanceIds = new Set(this.instances.map((instance) => instance.instanceId));
        this.nodes = this.nodes.filter((node) => !node.instanceId || validInstanceIds.has(node.instanceId));
        if (
            this.selectedInstanceId &&
            !this.instances.some((instance) => instance.instanceId === this.selectedInstanceId)
        ) {
            this.selectedInstanceId = null;
        }
        this.postInstances();
        this.postNodes();
    }

    setSelectedInstanceId(instanceId: string | null): void {
        this.selectedInstanceId = instanceId;
        this.postSelectedInstance();
    }

    setPackageRoot(packageRoot: string | null): void {
        this.packageRoot = packageRoot;
        if (!this.view) {
            return;
        }
        const message: LiveGraphSetPackageRootMessage = {
            type: "setPackageRoot",
            packageRoot,
        };
        void this.view.webview.postMessage(message);
    }

    upsertNodes(nodes: VirtualNode[], replaceInstanceIds: string[] = []): void {
        const replaceIds = new Set(replaceInstanceIds);
        const nextNodes = this.nodes.filter((node) => !node.instanceId || !replaceIds.has(node.instanceId));
        const nextById = new Map(nextNodes.map((node) => [node.id || "", node]));
        for (const node of nodes) {
            if (!node.id) {
                continue;
            }
            nextById.set(node.id, node);
        }
        this.nodes = [...nextById.values()];
        this.postNodeDelta(nodes, replaceInstanceIds);
    }

    setControlHandler(handler: LiveGraphControlHandler): void {
        this.controlHandler = handler;
    }

    resolveWebviewView(webviewView: vscode.WebviewView): void {
        this.view = webviewView;
        const builtMediaRoot = vscode.Uri.file(path.join(__dirname, "media"));
        webviewView.webview.options = {
            enableScripts: true,
            localResourceRoots: [
                vscode.Uri.joinPath(this.extensionUri, "media"),
                builtMediaRoot,
            ],
        };
        webviewView.webview.onDidReceiveMessage(async (message) => {
            if (!isLiveGraphControlMessage(message)) {
                return;
            }
            if (message.type === "selectInstance") {
                this.selectedInstanceId = message.instanceId ?? null;
                this.postSelectedInstance();
            }
            if (!this.controlHandler) {
                return;
            }
            await this.controlHandler(message);
        });
        webviewView.webview.html = this.getHtml(webviewView.webview);
        this.postInstances();
        this.setPackageRoot(this.packageRoot);
        this.postSelectedInstance();
        this.postNodes();
    }

    private postInstances(): void {
        if (!this.view) {
            return;
        }
        const message: LiveGraphSetInstancesMessage = {
            type: "setInstances",
            instances: serializeLiveGraphInstances(this.instances),
        };
        void this.view.webview.postMessage(message);
    }

    private postSelectedInstance(): void {
        if (!this.view) {
            return;
        }
        const message: LiveGraphSetSelectedInstanceMessage = {
            type: "setSelectedInstance",
            selectedInstanceId: this.selectedInstanceId,
        };
        void this.view.webview.postMessage(message);
    }

    private postNodes(): void {
        if (!this.view) {
            return;
        }
        const message: LiveGraphSetNodesMessage = {
            type: "setNodes",
            nodes: serializeLiveGraphNodes(this.filteredNodes()),
        };
        void this.view.webview.postMessage(message);
    }

    private postNodeDelta(nodes: VirtualNode[], replaceInstanceIds: string[]): void {
        if (!this.view) {
            return;
        }
        const filteredNodes = nodes.filter((node) => {
            if (!this.selectedInstanceId) {
                return true;
            }
            return node.instanceId === this.selectedInstanceId;
        });
        const filteredReplaceInstanceIds = !this.selectedInstanceId
            ? replaceInstanceIds
            : replaceInstanceIds.filter((instanceId) => instanceId === this.selectedInstanceId);
        const message: LiveGraphUpsertNodesMessage = {
            type: "upsertNodes",
            replaceInstanceIds: filteredReplaceInstanceIds,
            nodes: serializeLiveGraphNodes(filteredNodes),
        };
        void this.view.webview.postMessage(message);
    }

    private filteredNodes(): VirtualNode[] {
        if (!this.selectedInstanceId) {
            return this.nodes;
        }

        const selectedNodes = this.nodes.filter((node) => node.instanceId === this.selectedInstanceId);
        return selectedNodes.length > 0 ? selectedNodes : this.nodes;
    }

    getHtml(webview: vscode.Webview): string {
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

        .toolbar {
            display: flex;
            align-items: center;
            gap: 8px;
            padding: 8px 10px;
            border-bottom: 1px solid var(--vscode-sideBar-border, var(--vscode-widget-border));
            background: color-mix(in srgb, var(--vscode-sideBar-background) 90%, var(--vscode-editor-background));
        }

        .toolbar-label {
            color: var(--vscode-descriptionForeground);
            flex: 0 0 auto;
        }

        .toolbar-select {
            min-width: 0;
            flex: 1 1 auto;
            border: 1px solid var(--vscode-dropdown-border, transparent);
            background: var(--vscode-dropdown-background);
            color: var(--vscode-dropdown-foreground);
            font: inherit;
            padding: 4px 6px;
        }

        .toolbar-select:focus {
            outline: 1px solid var(--vscode-focusBorder);
            outline-offset: 0;
        }

        .content {
            display: flex;
            flex-direction: column;
            min-height: 0;
            flex: 1 1 auto;
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

        .port-row[data-direction="input"] .port-icon {
            color: var(--vscode-terminal-ansiYellow);
            opacity: 0.45;
        }

        .port-row[data-direction="output"] .port-icon {
            color: var(--vscode-terminal-ansiBlue);
            opacity: 0.45;
        }

        .port-row[data-active="true"] .port-icon {
            opacity: 0.95;
        }

    </style>
</head>
<body>
    <div id="root">
        <div class="toolbar">
            <div class="toolbar-label">instance</div>
            <select id="instanceSelect" class="toolbar-select" aria-label="Selected iv module instance"></select>
            <button id="createInstance" class="toolbar-button" type="button">Duplicate</button>
        </div>
        <div id="content" class="content"></div>
    </div>
    <script nonce="${nonce}">
        const vscode = acquireVsCodeApi();
        const content = document.getElementById("content");
        const instanceSelect = document.getElementById("instanceSelect");
        const createInstance = document.getElementById("createInstance");
        const state = {
            instances: [],
            selectedInstanceId: null,
            packageRoot: null,
            nodes: [],
            expanded: new Map(),
            portRows: new Map(),
            renderedPortRows: new Set(),
        };
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

        function renderPort(parent, nodeId, memberIndex, group, port, index) {
            const memberKeyPart = memberIndex == null ? "" : \`/member:\${memberIndex}\`;
            const portKey = \`node:\${nodeId}\${memberKeyPart}/group:\${group.label}/port:\${index}\`;
            const portDescription = [
                port.typeSummary,
                port.bindingSummary,
                port.connectivity,
            ]
                .filter((value) => typeof value === "string" && value.length > 0)
                .join(" • ");
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
            descriptionEl.textContent = portDescription;

            portRow.className = "tree-row port-row";
            if (memberIndex != null) {
                portRow.classList.add("member-port-row");
            }
            portRow.dataset.direction = group.direction;
            portRow.dataset.connectivity = port.connectivity;
            portRow.dataset.binding = port.binding;
            portRow.dataset.active = String(
                port.binding !== "none" || port.connectivity !== "disconnected");
            const icon = portRow.__portIcon;
            icon.className = group.direction === "input" ? "port-icon" : "port-icon mirrored";
            icon.innerHTML = icons.arrowRight;

            portRow.title = \`\${port.name} • \${portDescription || port.connectivity}\`;
            parent.appendChild(portRow);
            return portKey;
        }

        function renderGroup(parent, nodeId, memberIndex, group) {
            const memberKeyPart = memberIndex == null ? "" : \`/member:\${memberIndex}\`;
            const groupKey = \`node:\${nodeId}\${memberKeyPart}/group:\${group.label}\`;
            const isExpanded = expandedValue(groupKey, true);

            const groupEl = document.createElement("div");
            groupEl.className = isExpanded ? "group" : "group collapsed";

            const header = row(group.label, String(group.count));
            header.classList.add("group-header");
            if (memberIndex != null) {
                header.classList.add("member-group-header");
            }
            header.prepend(disclosure(isExpanded));
            header.addEventListener("click", () => toggleExpanded(groupKey, true));
            groupEl.appendChild(header);

            const ports = document.createElement("div");
            ports.className = "group-ports";
            for (let i = 0; i < group.ports.length; ++i) {
                renderPort(ports, nodeId, memberIndex, group, group.ports[i], i);
            }
            groupEl.appendChild(ports);
            parent.appendChild(groupEl);
        }

        function renderMember(parent, node, member) {
            const memberKey = \`node:\${node.id}/member:\${member.index}\`;
            const isExpanded = expandedValue(memberKey, false);

            const memberEl = document.createElement("div");
            memberEl.className = isExpanded ? "group" : "group collapsed";

            const header = row(member.kind || "concrete", member.description || \`concrete \${member.index}\`);
            header.classList.add("member-header");
            header.prepend(disclosure(isExpanded));
            header.addEventListener("click", () => toggleExpanded(memberKey, false));
            memberEl.appendChild(header);

            const groupsEl = document.createElement("div");
            groupsEl.className = "group-ports";
            for (const group of member.groups) {
                renderGroup(groupsEl, node.id, member.index, group);
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

            const header = row("concrete nodes", String(node.members.length));
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

        function renderInstanceSelect() {
            const previousValue = instanceSelect.value;
            instanceSelect.textContent = "";

            const placeholder = document.createElement("option");
            placeholder.value = "";
            placeholder.textContent = state.instances.length > 0
                ? "[all available instances]"
                : "[no instances]";
            instanceSelect.appendChild(placeholder);

            for (const instance of state.instances) {
                const option = document.createElement("option");
                option.value = instance.instanceId;
                option.textContent = instance.label || instance.instanceId;
                if (!instance.realized) {
                    option.textContent += " [loading]";
                }
                instanceSelect.appendChild(option);
            }

            const selectedValue = state.selectedInstanceId || "";
            instanceSelect.value = [...instanceSelect.options].some((option) => option.value === selectedValue)
                ? selectedValue
                : "";
            if (instanceSelect.value !== previousValue) {
                instanceSelect.title = instanceSelect.selectedOptions[0]?.textContent || "";
            }
            instanceSelect.disabled = state.instances.length === 0;
            createInstance.disabled = !state.packageRoot;
        }

        function render() {
            state.renderedPortRows.clear();
            renderInstanceSelect();
            content.textContent = "";
            if (!Array.isArray(state.nodes) || state.nodes.length === 0) {
                const empty = document.createElement("div");
                empty.className = "empty";
                empty.textContent = "[no nodes]";
                empty.title = "No visible virtual nodes at the current selection";
                content.appendChild(empty);
                return;
            }

            for (const node of state.nodes) {
                renderNode(content, node);
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
            if (message.type === "setInstances") {
                state.instances = Array.isArray(message.instances) ? message.instances : [];
            } else if (message.type === "setSelectedInstance") {
                state.selectedInstanceId = typeof message.selectedInstanceId === "string"
                    ? message.selectedInstanceId
                    : null;
            } else if (message.type === "setPackageRoot") {
                state.packageRoot = typeof message.packageRoot === "string" ? message.packageRoot : null;
            } else if (message.type === "setNodes") {
                state.nodes = Array.isArray(message.nodes) ? message.nodes : [];
            } else if (message.type === "upsertNodes") {
                const replaceInstanceIds = new Set(Array.isArray(message.replaceInstanceIds) ? message.replaceInstanceIds : []);
                const nextNodes = state.nodes.filter((node) => !replaceInstanceIds.has(node.instanceId));
                const nextById = new Map(nextNodes.map((node) => [node.id, node]));
                for (const node of Array.isArray(message.nodes) ? message.nodes : []) {
                    nextById.set(node.id, node);
                }
                state.nodes = [...nextById.values()];
            } else {
                return;
            }
            render();
        });

        instanceSelect.addEventListener("change", () => {
            const instanceId = instanceSelect.value || null;
            state.selectedInstanceId = instanceId;
            instanceSelect.title = instanceSelect.selectedOptions[0]?.textContent || "";
            vscode.postMessage({
                type: "selectInstance",
                instanceId,
            });
        });

        createInstance.addEventListener("click", () => {
            vscode.postMessage({ type: "createInstance" });
        });

        render();
    </script>
</body>
</html>`;
    }
}
