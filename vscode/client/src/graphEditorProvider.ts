import * as crypto from "node:crypto";
import * as vscode from "vscode";
import { GraphLayoutStore } from "./graphLayoutStore";
import { addressKey, readLayout, type GraphScope } from "./graphScope";
import { nodeDebugDetails } from "./nodeDebugDetails";
import { ObservationDeliveryState, type ObservationDelivery } from "./observationDelivery";
import type { GraphEditorAction, GraphEditorHostMessage, GraphEditorState, GraphEditorWebviewMessage } from "./graphEditorProtocol";
import type { WorkspaceSession } from "./workspaceSession";
import { formatStatusHint, isStatusHint, playbackShortcut, rightClickHint, type StatusHint } from "./statusBarText";
import { isNodeEventRequest, isNodeEventGestureRequest, replyToNodeEvent, replyToNodeEventGesture } from "./nodeEventMessaging";
import { isPendingPlaybackSeek, playbackSeekPosition, type PlaybackDrag, type PlaybackDragPhase,
    type PlaybackSeek, type PlaybackSnapshot } from "./playback";
import type { NativeClientHelper } from "./nativeClientHelper";

export class GraphEditorProvider implements vscode.Disposable {
    private readonly panels = new Set<vscode.WebviewPanel>();
    private readonly interactionHints = new Map<vscode.WebviewPanel, StatusHint>();
    private inspectionHint: StatusHint | null = null;
    private readonly interactionStatus = vscode.window.createStatusBarItem(
        "intravenous.manualControlHints", vscode.StatusBarAlignment.Left, 10);
    private readonly controlOwners = new Map<vscode.WebviewPanel, string>();
    private readonly controlsChanged: vscode.Disposable;
    private readonly observationsChanged: vscode.Disposable;
    private readonly observations = new ObservationDeliveryState();
    private readonly panelObservations = new Map<vscode.WebviewPanel, number>();
    private nextObservationSequence = 0;
    private readonly ready = new Set<vscode.WebviewPanel>();
    private lastActivePanel: vscode.WebviewPanel | null = null;
    private readonly pendingActions = new Map<vscode.WebviewPanel, GraphEditorAction[]>();
    private readonly store: GraphLayoutStore;
    private readonly layoutLoaded: Promise<void>;
    private readonly changed: vscode.Disposable;
    private readonly windowFocusChanged: vscode.Disposable;
    private readonly shortcutSettingChanged: vscode.Disposable;
    private state: GraphEditorState = {
        sessionStatus: "starting", scope: null, layout: readLayout(null), definitions: [], error: "",
    };
    private dirty = false;
    private reading = false;
    private disposed = false;
    private layoutError = "";
    private playbackReading = false;
    private pendingSeek: PlaybackSeek | null = null;
    private playbackDrag: { panel: vscode.WebviewPanel; dragId: string; position: string } | null = null;
    private playbackSnapshot: PlaybackSnapshot | null = null;
    private readonly playbackTimer = setInterval(() => { void this.refreshPlayback(); }, 50);

    private postPlayback(): void {
        if (this.disposed) return;
        if (this.pendingSeek && !isPendingPlaybackSeek(this.pendingSeek, this.playbackSnapshot)) this.pendingSeek = null;
        for (const panel of this.ready) if (panel.visible) void panel.webview.postMessage({
            type: "playbackUpdate", snapshot: this.playbackSnapshot, requestedPosition: this.pendingSeek?.position ?? null,
        });
    }

    private async seekPlayback(position: string, drag?: PlaybackDrag): Promise<void> {
        const request: PlaybackSeek = { position };
        this.pendingSeek = request;
        this.postPlayback();
        try {
            const accepted = await this.session.playbackCommand(position, drag);
            if (this.pendingSeek !== request) return;
            if (accepted?.acceptedCommand === undefined) this.pendingSeek = null;
            else request.acceptedCommand = accepted.acceptedCommand;
        } catch (error) {
            if (this.pendingSeek === request) this.pendingSeek = null;
            throw error;
        } finally { this.postPlayback(); }
    }

    private async dragPlayback(panel: vscode.WebviewPanel, phase: PlaybackDragPhase, position: string): Promise<void> {
        if (phase === "begin") {
            if (!panel.visible) return;
            this.finishPlaybackDrag();
            this.playbackDrag = { panel, dragId: crypto.randomBytes(8).readBigUInt64BE().toString(), position };
        }
        const drag = this.playbackDrag;
        if (drag?.panel !== panel) return;
        drag.position = position;
        if (phase === "end") this.playbackDrag = null;
        try { await this.seekPlayback(position, { phase, dragId: drag.dragId }); }
        catch (error) {
            if (phase === "begin" && this.playbackDrag === drag) this.playbackDrag = null;
            throw error;
        }
    }

    private finishPlaybackDrag(panel = this.playbackDrag?.panel): void {
        const drag = this.playbackDrag;
        if (drag && drag.panel === panel) {
            void this.dragPlayback(drag.panel, "end", drag.position).catch((error) => {
                void vscode.window.showErrorMessage(`Cannot finish playback drag: ${error.message}`);
            });
        }
    }

    private async refreshPlayback(): Promise<void> {
        if (this.disposed || this.playbackReading || ![...this.ready].some((panel) => panel.visible)) return;
        this.playbackReading = true;
        try {
            this.playbackSnapshot = await this.session.readPlayback();
            if (!this.playbackSnapshot) this.pendingSeek = null;
        } catch {
            this.playbackSnapshot = null;
        } finally {
            this.playbackReading = false;
            this.postPlayback();
        }
    }

    async playbackAction(seek = false): Promise<void> {
        if (!seek) { await this.session.playbackCommand(); return; }
        const state = await this.session.readPlayback();
        if (!state) throw new Error("The Intravenous server is unavailable");
        const value = await vscode.window.showInputBox({ title: "Playback position", value: state.position,
            prompt: "Integer sample position, or seconds followed by s", validateInput: (text) => {
                try { playbackSeekPosition(text, state.sampleRate); return undefined; }
                catch (error) { return String(error); }
            } });
        if (value !== undefined) await this.seekPlayback(playbackSeekPosition(value, state.sampleRate));
    }

    constructor(private readonly extensionUri: vscode.Uri,
        private readonly session: WorkspaceSession, private readonly nativeHelper?: NativeClientHelper,
        private readonly log: (message: string) => void = () => {}) {
        this.store = new GraphLayoutStore(session.workspaceRoot());
        this.layoutLoaded = this.store.load().then((layout) => { this.state.layout = layout; })
            .catch((error) => {
                this.layoutError = `Cannot read graph layout: ${error.message}. Positions will not be saved until the file is repaired and the window reloaded.`;
                this.state.error = this.layoutError;
            });
        this.changed = session.onDidChangeGraph(() => {
            if (session.graphSessionStatus !== "running") {
                this.observations.clear();
                this.panelObservations.clear();
            }
            void this.refresh();
        });
        this.controlsChanged = session.onDidChangeManualControls(() => {
            for (const panel of this.ready) this.postControls(panel);
        });
        this.observationsChanged = session.onDidChangeObservations((update) => {
            if (session.graphSessionStatus !== "running") return;
            this.observations.receive(update);
            for (const panel of this.ready) {
                const owner = this.controlOwners.get(panel);
                if (!owner || !panel.visible) continue;
                const scope = update.scopes[owner];
                if (update.snapshot || !this.panelObservations.has(panel)) this.postObservationSnapshot(panel);
                else this.postObservationUpdate(panel, { ...update, scopes: scope === undefined ? {} : { viewport: scope } });
            }
        });
        this.windowFocusChanged = vscode.window.onDidChangeWindowState(({ focused }) => {
            if (!focused) {
                this.interactionHints.clear();
                this.inspectionHint = null;
                this.finishPlaybackDrag();
            }
            this.updateInteractionStatus();
            for (const panel of this.ready) void panel.webview.postMessage({ type: "graphWindowFocus", focused });
        });
        this.shortcutSettingChanged = vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration("intravenous.hideStatusBarShortcuts")) this.updateInteractionStatus();
        });
    }

    private postObservationUpdate(panel: vscode.WebviewPanel, update: ObservationDelivery): void {
        const previousSequence = this.panelObservations.get(panel);
        const sequence = ++this.nextObservationSequence;
        this.panelObservations.set(panel, sequence);
        const message: GraphEditorHostMessage = { type: "observationUpdate", update, sequence, previousSequence };
        const failed = () => {
            if (this.panelObservations.get(panel) === sequence) this.panelObservations.delete(panel);
        };
        void panel.webview.postMessage(message).then((posted) => { if (!posted) failed(); }, failed);
    }

    private postObservationSnapshot(panel: vscode.WebviewPanel): void {
        const owner = this.controlOwners.get(panel);
        if (!owner || !panel.visible || !this.ready.has(panel)) return;
        const snapshot = this.observations.snapshot(owner, "viewport");
        if (snapshot) this.postObservationUpdate(panel, snapshot);
    }

    private updateInteractionStatus(): void {
        if (!vscode.window.state.focused) {
            this.interactionStatus.hide();
            return;
        }
        const panel = [...this.panels].find((candidate) => candidate.active);
        const hint = (panel ? this.interactionHints.get(panel) : undefined) ?? this.inspectionHint;
        const hideShortcuts = vscode.workspace.getConfiguration("intravenous").get("hideStatusBarShortcuts", false);
        const text = hint ? formatStatusHint(hint, hideShortcuts) : "";
        if (!text && (!panel || hideShortcuts)) {
            this.interactionStatus.hide();
            return;
        }
        this.interactionStatus.command = text ? undefined : "intravenous.togglePlayback";
        this.interactionStatus.text = text || playbackShortcut;
        this.interactionStatus.accessibilityInformation = {
            label: text ? text.replaceAll(rightClickHint, "Right-click").replaceAll("↑↓", "up/down arrows")
                .replaceAll("▶/Ⅱ", "play or pause") : "Space to play or pause",
        };
        this.interactionStatus.show();
    }

    setInspectionHint(hint: StatusHint | null): void {
        if (this.disposed) return;
        this.inspectionHint = vscode.window.state.focused ? hint : null;
        this.updateInteractionStatus();
    }

    open(): vscode.WebviewPanel {
        const panel = vscode.window.createWebviewPanel("intravenous.graphEditor",
            "Intravenous Graph", vscode.ViewColumn.Active, { enableScripts: true, retainContextWhenHidden: true });
        this.attach(panel);
        return panel;
    }

    async profileObservations(): Promise<void> {
        const panel = [...this.ready].find((entry) => entry.active);
        if (!panel) {
            void vscode.window.showInformationMessage("Open the Intravenous Graph editor before profiling observation updates.");
            return;
        }
        const durationMs = 10_000;
        const request: GraphEditorHostMessage = { type: "startObservationProfile", durationMs };
        try {
            if (!await panel.webview.postMessage(request)) {
                void vscode.window.showInformationMessage("The Intravenous Graph editor could not start the observation profile.");
                return;
            }
            const message = `Recording observation updates for ${durationMs / 1000} seconds. See Intravenous output for the report.`;
            this.log(message);
            vscode.window.setStatusBarMessage(message, durationMs);
        } catch (error) {
            void vscode.window.showErrorMessage(`Cannot profile observation updates: ${error}`);
        }
    }

    async runAction(action: GraphEditorAction, target?: vscode.WebviewPanel): Promise<void> {
        const panel = target ?? (this.lastActivePanel && this.panels.has(this.lastActivePanel)
            ? this.lastActivePanel : [...this.panels].find((entry) => entry.active) ?? [...this.panels][0] ?? this.open());
        panel.reveal(panel.viewColumn, false);
        if (!this.ready.has(panel)) {
            this.pendingActions.set(panel, [...(this.pendingActions.get(panel) ?? []), action]);
            return;
        }
        if (action === "undo" || action === "redo") {
            await this.session.projectHistory(action);
            await this.refresh();
            return;
        }
        const [scope, definitions] = await Promise.all([this.session.readGraphScope(), this.session.graphDefinitions()]);
        if (!this.panels.has(panel)) return;
        this.session.manualControls.reconcile(scope);
        this.state = { ...this.state, scope, definitions, sessionStatus: this.session.graphSessionStatus };
        this.post(panel);
        if (action === "addNode" || action === "createPackage") {
            void panel.webview.postMessage({ type: "graphAction", action });
            return;
        }
        if (action === "deleteConnection") {
            const label = (matcher: GraphScope["connections"][number]["connection"]["outputs"][number]) => {
                const instance = scope.nodes.find((node) => node.instance.instanceId === matcher.instanceId)?.instance;
                return `${instance?.displayName || instance?.definitionId.split(".").pop() || "Unavailable node"}.${matcher.port.name ?? matcher.port.index ?? "*"}`;
            };
            const selected = await vscode.window.showQuickPick(scope.connections.map((entry) => ({
                label: `${entry.connection.outputs.map(label).join(", ")} → ${entry.connection.inputs.map(label).join(", ")}`,
                description: entry.applied ? "" : "Not applied", id: entry.id,
            })), { title: "Intravenous: Delete Connection", placeHolder: "Select a connection" });
            if (selected) { await this.session.deleteGraphConnection(selected.id); await this.refresh(); }
            return;
        }
        if (action === "deleteNode") {
            const selected = await vscode.window.showQuickPick(scope.nodes.map((node) => ({
                label: node.instance.displayName || node.instance.definitionId.split(".").pop() || node.instance.definitionId,
                description: node.instance.definitionId, instanceId: node.instance.instanceId,
            })), { title: "Intravenous: Delete Nodes", placeHolder: "Select nodes to delete", canPickMany: true });
            if (selected?.length && this.panels.has(panel)) {
                await this.session.deleteGraphNodes(selected.map((node) => node.instanceId));
                await this.refresh();
            }
            return;
        }
        const selected = await vscode.window.showQuickPick(scope.nodes.map((node, index) => ({
            label: node.instance.displayName || node.instance.definitionId.split(".").pop() || node.instance.definitionId,
            description: node.instance.definitionId, instanceId: node.instance.instanceId,
            detail: `Lane ${index + 1}${node.instance.configurationSource ? ` · ${node.instance.configurationSource}` : ""}`,
        })), { title: `Intravenous: ${action === "editNode" ? "Edit Node" : "Open Node Source"}`,
            placeHolder: "Select a node" });
        if (!selected || !this.panels.has(panel)) return;
        if (action === "editNode") {
            const current = await this.session.readGraphScope();
            if (!current.nodes.some((node) => node.instance.instanceId === selected.instanceId)) {
                throw new Error("The selected node is no longer available");
            }
            this.state.scope = current;
            this.post(panel);
            void panel.webview.postMessage({ type: "graphAction", action, instanceId: selected.instanceId });
        } else {
            await this.session.openGraphNodeSource(selected.instanceId);
        }
    }

    // VS Code restores each tab independently after a window reload.
    revive(panel: vscode.WebviewPanel): void { this.attach(panel); }

    private attach(panel: vscode.WebviewPanel): void {
        this.panels.add(panel);
        this.controlOwners.set(panel, crypto.randomUUID());
        if (panel.active) this.lastActivePanel = panel;
        const viewState = panel.onDidChangeViewState(() => {
            void panel.webview.postMessage({ type: "observationVisibility", visible: panel.visible });
            if (!panel.visible) {
                this.panelObservations.delete(panel);
                this.finishPlaybackDrag(panel);
                const owner = this.controlOwners.get(panel);
                if (owner) void this.session.reconcileObservations(owner, []).catch(() => {});
            } else if (!this.panelObservations.has(panel)) this.postObservationSnapshot(panel);
            if (panel.active) this.lastActivePanel = panel;
            else this.interactionHints.delete(panel);
            this.updateInteractionStatus();
        });
        const root = vscode.Uri.joinPath(this.extensionUri, "dist", "webview");
        panel.webview.options = { enableScripts: true, localResourceRoots: [root] };
        const messages = panel.webview.onDidReceiveMessage(async (message: GraphEditorWebviewMessage) => {
            if (message?.type === "observationDescribe" || message?.type === "observationReconcile") {
                const owner = this.controlOwners.get(panel);
                if (!owner) return;
                try {
                    const result = message.type === "observationDescribe"
                        ? await this.session.describeObservations(message.nodes)
                        : await this.session.reconcileObservations(owner, panel.visible ? message.interests : []);
                    if (this.panels.has(panel)) void panel.webview.postMessage({ type: "observationResponse", requestId: message.requestId, result });
                } catch (error) {
                    if (this.panels.has(panel)) void panel.webview.postMessage({ type: "observationResponse", requestId: message.requestId, error: String(error) });
                }
            } else if (message?.type === "observationResync") {
                this.postObservationSnapshot(panel);
            } else if (message?.type === "observationProfile") {
                const s = message.summary;
                const updateRate = (s.updates / s.seconds).toFixed(2);
                const dataRate = (s.messageBytes / 1024 / s.seconds).toFixed(2);
                this.log(`Observation profile over ${s.seconds.toFixed(2)} s`);
                this.log(`  Webview received ${s.updates} updates (${updateRate}/s, about ${dataRate} KiB/s as JSON).`
                    + ` ${s.presentationUpdates} triggered a presentation update.`);
                this.log(`  ${s.snapshots} snapshots, ${s.metadataOnly} updates with only frame metadata,`
                    + ` ${s.liveValues} live readings, and ${s.removals} removals.`);
                this.log(`  ${s.windowChanges} window updates (${s.statusChanges} included status),`
                    + ` ${s.windowBodies} included sample or event data (${(s.windowBytes / 1024).toFixed(2)} KiB).`);
                this.log(`  Measurement took ${s.measurementMs.toFixed(2)} ms in the webview.`);
            } else if (isNodeEventRequest(message) || isNodeEventGestureRequest(message)) {
                const owner = this.controlOwners.get(panel);
                if (!owner) return;
                const response = message.type === "sendNodeEvent"
                    ? await replyToNodeEvent(message, (target, event, id) => this.session.nodeEvents.send(owner, target, event, id))
                    : await replyToNodeEventGesture(message, (target, id, begin) => begin
                        ? this.session.nodeEvents.begin(owner, target, id) : this.session.nodeEvents.end(owner, target, id));
                if (this.panels.has(panel)) void panel.webview.postMessage(response);
                if (response.error) void vscode.window.showErrorMessage(`Cannot send node event: ${response.error}`);
            } else if (message?.type === "cursorReturnSkipped" && typeof message.detail === "string") {
                this.log(`Cursor return skipped. ${message.detail}`);
            } else if (message?.type === "warpCursorAfterPointDrag" || message?.type === "warpCursorAfterMiddleDrag") {
                const skipped = !panel.active || !vscode.window.state.focused ? "graph editor lost focus"
                    : message.type === "warpCursorAfterPointDrag"
                        && !vscode.workspace.getConfiguration("intravenous").get("warpCursorAfterPointDrag", true) ? "cursor return is disabled"
                    : !this.nativeHelper ? "no local native helper" : null;
                if (skipped) {
                    if (skipped === "graph editor lost focus") this.log(`Cursor return skipped. ${skipped}.`);
                } else this.nativeHelper?.warpCursor(message.warp, message.type === "warpCursorAfterMiddleDrag", () => {
                    if (this.panels.has(panel)) void panel.webview.postMessage({ type: "cursorReturnCompleted", requestId: message.requestId });
                });
            } else if (message?.type === "playbackToggle" || message?.type === "playbackSeek" || message?.type === "playbackDrag") {
                try {
                    if (message.type === "playbackDrag") {
                        await this.dragPlayback(panel, message.phase, playbackSeekPosition(message.position, 1));
                    } else if (message.type === "playbackSeek") await this.seekPlayback(playbackSeekPosition(message.position, 1));
                    else await this.session.playbackCommand();
                }
                catch (error) { void vscode.window.showErrorMessage(`Cannot change playback: ${error.message}`); }
            } else if (message?.type === "graphReady") {
                await this.layoutLoaded;
                if (!this.panels.has(panel)) return;
                this.interactionHints.delete(panel);
                this.updateInteractionStatus();
                if (this.ready.has(panel)) {
                    this.finishPlaybackDrag(panel);
                    const owner = this.controlOwners.get(panel);
                    if (owner) { this.session.manualControls.releaseOwner(owner); this.session.nodeEvents.releaseOwner(owner); }
                }
                this.ready.add(panel);
                this.panelObservations.delete(panel);
                void panel.webview.postMessage({ type: "observationVisibility", visible: panel.visible });
                this.postControls(panel);
                void panel.webview.postMessage({ type: "graphWindowFocus", focused: vscode.window.state.focused });
                this.post(panel);
                this.postObservationSnapshot(panel);
                await this.refresh();
                const pending = this.pendingActions.get(panel) ?? [];
                this.pendingActions.delete(panel);
                for (const action of pending) {
                    try { await this.runAction(action, panel); }
                    catch (error) { void vscode.window.showErrorMessage(`Cannot edit project: ${error.message}`); }
                }
            } else if (message?.type === "graphInteractionHint") {
                if (isStatusHint(message.hint) && panel.active && vscode.window.state.focused) {
                    this.interactionHints.set(panel, message.hint);
                } else this.interactionHints.delete(panel);
                this.updateInteractionStatus();
            } else if (message?.type === "manualControlAction") {
                const owner = this.controlOwners.get(panel);
                if (!owner) return;
                try { this.session.manualControls.action(owner, message.action); }
                catch (error) {
                    if (message.action.action === "begin") void panel.webview.postMessage({
                        type: "manualControlState", interactions: [...this.session.manualControls.states(owner), {
                            interactionId: message.action.interactionId, control: message.action.control,
                            value: message.action.value, phase: "failed", error: error.message,
                            graphGeneration: this.state.scope?.generation,
                        }], live: this.session.manualControls.live(owner),
                    });
                    void vscode.window.showErrorMessage(`Cannot edit manual input: ${error.message}`);
                }
            } else if (message?.type === "deleteSelectedLanes") {
                try {
                    await this.session.deleteGraphNodes(message.instanceIds);
                    await this.refresh();
                } catch (error) { void vscode.window.showErrorMessage(`Cannot delete lanes: ${error.message}`); }
            } else if (message?.type === "setManualInputEnabled") {
                try { await this.session.setManualInputEnabled(message.target, message.inputName, message.enabled); }
                catch (error) { void vscode.window.showErrorMessage(`Cannot change manual input: ${error.message}`); }
            } else if (message?.type === "projectHistory") {
                try {
                    await this.session.projectHistory(message.direction);
                    await this.refresh();
                } catch (error) { void vscode.window.showErrorMessage(`Cannot ${message.direction} project edit: ${error.message}`); }
            } else if (message?.type === "copyNodeDebugDetails") {
                const scope = this.state.scope;
                const node = scope?.nodes.find((entry) => addressKey(entry.address) === message.nodeKey);
                if (!scope || !node) return;
                try { await vscode.env.clipboard.writeText(nodeDebugDetails(scope, node)); }
                catch (error) { void vscode.window.showErrorMessage(`Cannot copy node details: ${error.message}`); }
            } else if (message?.type === "connectGraphPorts" || message?.type === "editGraphConnectionEnd") {
                try {
                    if (message.type === "connectGraphPorts") {
                        await this.session.connectGraphPorts(crypto.randomUUID(), message.first, message.second);
                    } else await this.session.editGraphConnectionEnd(message.edit);
                    await this.refresh();
                } catch (error) {
                    void vscode.window.showErrorMessage(`Cannot edit connection: ${error.message}`);
                }
            } else if (message?.type === "createGraphNode" || message?.type === "updateGraphNode"
                || message?.type === "createGraphPackage") {
                try {
                    if (message.type === "createGraphNode") {
                        await this.session.createGraphNode(message.definitionId, message.displayName, message.configurationSource);
                    } else if (message.type === "updateGraphNode") {
                        await this.session.updateGraphNode(message.instanceId, message.displayName, message.configurationSource,
                            message.previousDisplayName, message.previousConfigurationSource);
                    } else {
                        await this.session.createGraphPackage(message.name);
                    }
                    await this.refresh();
                } catch (error) {
                    void vscode.window.showErrorMessage(`Cannot edit project: ${error.message}`);
                }
            } else if (message?.type === "saveGraphLayout" && message.layout) {
                await this.layoutLoaded;
                if (this.layoutError) return;
                const next = readLayout(message.layout);
                this.state.layout = { ...next, positions: { ...this.state.layout.positions, ...next.positions },
                    presentationStates: this.state.layout.presentationStates, laneOrder: this.state.layout.laneOrder };
                try {
                    const view = await this.store.save(this.state.layout, message.presentationStates, message.laneOrder);
                    this.state.layout = { ...this.state.layout, ...view };
                }
                catch (error) {
                    void vscode.window.showErrorMessage(`Cannot save graph layout: ${error.message}`);
                }
            }
        });
        panel.onDidDispose(() => {
            this.finishPlaybackDrag(panel);
            const owner = this.controlOwners.get(panel);
            if (owner) {
                this.session.manualControls.releaseOwner(owner); this.session.nodeEvents.releaseOwner(owner);
                void this.session.reconcileObservations(owner, []).catch(() => {});
            }
            this.controlOwners.delete(panel);
            this.panelObservations.delete(panel);
            this.interactionHints.delete(panel);
            messages.dispose(); viewState.dispose(); this.panels.delete(panel); this.ready.delete(panel);
            this.updateInteractionStatus();
            this.pendingActions.delete(panel);
            if (this.lastActivePanel === panel) this.lastActivePanel = null;
        });
        const nonce = crypto.randomBytes(16).toString("base64");
        const script = panel.webview.asWebviewUri(vscode.Uri.joinPath(root, "main.js"));
        const css = panel.webview.asWebviewUri(vscode.Uri.joinPath(root, "main.css"));
        panel.webview.html = `<!doctype html><html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src ${panel.webview.cspSource} data:; style-src ${panel.webview.cspSource} 'unsafe-inline'; script-src 'nonce-${nonce}'">
<link rel="stylesheet" href="${css}"></head><body data-surface="graphEditor"><div id="app"></div>
<script type="module" nonce="${nonce}" src="${script}"></script></body></html>`;
    }

    private postControls(panel: vscode.WebviewPanel): void {
        const owner = this.controlOwners.get(panel);
        if (owner && this.ready.has(panel)) void panel.webview.postMessage({
            type: "manualControlState", interactions: this.session.manualControls.states(owner),
            live: this.session.manualControls.live(owner),
        });
    }

    private post(panel: vscode.WebviewPanel): void {
        if (this.ready.has(panel)) void panel.webview.postMessage({ type: "graphState", state: this.state });
    }

    private async refresh(): Promise<void> {
        if (!this.panels.size || this.disposed) return;
        this.dirty = true;
        if (this.reading) return;
        this.reading = true;
        try {
            await this.layoutLoaded;
            while (this.dirty && !this.disposed) {
                this.dirty = false;
                this.state.sessionStatus = this.session.graphSessionStatus;
                if (this.state.sessionStatus !== "running") {
                    this.state.scope = null;
                } else {
                    try {
                        const [scope, definitions]: [GraphScope, string[]] = await Promise.all([
                            this.session.readGraphScope(), this.session.graphDefinitions(),
                        ]);
                        // A change while reading requires another complete read.
                        // Do not show a stale response, especially after restart.
                        if (this.dirty || this.disposed) continue;
                        this.session.manualControls.reconcile(scope);
                        this.state.scope = scope;
                        this.state.definitions = definitions;
                        this.state.error = this.layoutError;
                    } catch (error) {
                        if (this.dirty) continue;
                        this.state.error = `Cannot read graph: ${error.message}`;
                    }
                }
                for (const panel of this.panels) this.post(panel);
            }
        } finally { this.reading = false; }
    }

    dispose(): void {
        this.disposed = true;
        clearInterval(this.playbackTimer);
        this.changed.dispose();
        this.controlsChanged.dispose();
        this.observationsChanged.dispose();
        this.windowFocusChanged.dispose();
        this.shortcutSettingChanged.dispose();
        for (const panel of this.panels) panel.dispose();
        this.panels.clear();
        this.interactionStatus.dispose();
    }
}
