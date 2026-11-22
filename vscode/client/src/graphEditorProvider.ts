import * as crypto from "node:crypto";
import { mkdtemp, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import * as vscode from "vscode";
import { isGeneratedSnapConnection, isTimelineSnapNode } from "./timelineSnap";
import { addressKey, graphScopeNodeLabel, graphScopeWithChildren, type GraphScope, type NodeAddress } from "./graphScope";
import { nodeDebugDetails } from "./nodeDebugDetails";
import { ObservationDeliveryState, type ObservationDelivery } from "./observationDelivery";
import { observationInterestReport } from "./observationProfile";
import { GraphUiTimings, graphUiProfileReport } from "./graphUiProfile";
import { answerObservationRequest } from "./observationEndpoint";
import { isInteractionSend, type ClientInput, type ClientInteractionTrace, type InteractionRecording, type Timed } from "./clientInteractionTrace";
import type { GraphEditorAction, GraphEditorHostMessage, GraphEditorState, GraphEditorWebviewMessage, TraceDetails } from "./graphEditorProtocol";
import type { WorkspaceSession } from "./workspaceSession";
import { formatStatusHint, isStatusHint, playbackShortcut, rightClickHint, type StatusHint } from "./statusBarText";
import { answerNodeEventRequest, isNodeEventRequest, isNodeEventGestureRequest } from "./nodeEventMessaging";
import { isPendingPlaybackSeek, playbackSeekPosition, type PlaybackDrag, type PlaybackDragPhase, type PlaybackOutputProfileResult,
    type PlaybackSeek, type PlaybackSnapshot } from "./playback";
import { PlaybackProfile } from "./playbackProfile";
import type { NativeClientHelper } from "./nativeClientHelper";
import { PresentationArtifactReader } from "./presentationArtifacts";
import type { PresentationArtifact } from "./presentationPlugin";

export class GraphEditorProvider implements vscode.Disposable {
    private readonly panels = new Set<vscode.WebviewPanel>();
    private readonly expandedByPanel = new Map<vscode.WebviewPanel, NodeAddress[]>();
    private readonly interactionHints = new Map<vscode.WebviewPanel, StatusHint>();
    private inspectionHint: StatusHint | null = null;
    // Keep lane details to the left of the transient build and compilation items.
    private readonly interactionStatus = vscode.window.createStatusBarItem(
        "intravenous.manualControlHints", vscode.StatusBarAlignment.Left, 101);
    private readonly controlOwners = new Map<vscode.WebviewPanel, string>();
    private readonly controlsChanged: vscode.Disposable;
    private readonly observationsChanged: vscode.Disposable;
    private readonly observations = new ObservationDeliveryState();
    private readonly panelObservations = new Map<vscode.WebviewPanel, number>();
    private clientTrace?: {
        id: string; panel: vscode.WebviewPanel; started: number; stop: () => void;
        timer: ReturnType<typeof setTimeout>; events: Timed<ClientInput>[];
        trace: Omit<ClientInteractionTrace, keyof InteractionRecording>;
    };
    private nextObservationSequence = 0;
    private readonly ready = new Set<vscode.WebviewPanel>();
    private lastActivePanel: vscode.WebviewPanel | null = null;
    private readonly graphUiProfiles = new Map<vscode.WebviewPanel, { id: string; waits: GraphUiTimings }>();
    private readonly pendingActions = new Map<vscode.WebviewPanel, GraphEditorAction[]>();
    private presentationCatalog?: Promise<PresentationArtifact[]>;
    private readonly presentationReader = new PresentationArtifactReader();
    private readonly presentationChanges: vscode.Disposable;
    private readonly changed: vscode.Disposable;
    private readonly windowFocusChanged: vscode.Disposable;
    private readonly shortcutSettingChanged: vscode.Disposable;
    private state: GraphEditorState = {
        sessionStatus: "starting", scope: null, definitions: null, definitionUsage: {}, error: "",
    };
    private dirty = false;
    private reading = false;
    private disposed = false;
    private playbackReading = false;
    private pendingSeek: (PlaybackSeek & { releasedFrom?: vscode.WebviewPanel }) | null = null;
    private playbackDrag: { panel: vscode.WebviewPanel; dragId: string; position: string } | null = null;
    private readonly dragReleaseIds = new WeakMap<vscode.WebviewPanel, string>();
    private playbackSnapshot: PlaybackSnapshot | null = null;
    private playbackProfile?: {
        id: string; panel: vscode.WebviewPanel; recorder: PlaybackProfile; timer: ReturnType<typeof setTimeout>;
    };
    private readonly playbackTimer = setInterval(() => { void this.refreshPlayback(); }, 50);

    private postPlayback(): void {
        if (this.disposed) return;
        if (this.pendingSeek && !isPendingPlaybackSeek(this.pendingSeek, this.playbackSnapshot)) this.pendingSeek = null;
        for (const panel of this.ready) if (panel.visible) void panel.webview.postMessage({
            type: "playbackUpdate", snapshot: this.playbackSnapshot, requestedPosition: this.pendingSeek?.position ?? null,
            releasedDragPosition: this.pendingSeek?.releasedFrom === panel ? this.pendingSeek.position : null,
            dragReleaseId: this.dragReleaseIds.get(panel),
        });
    }

    private async seekPlayback(position: string, drag?: PlaybackDrag, releasedFrom?: vscode.WebviewPanel): Promise<void> {
        const request: PlaybackSeek & { releasedFrom?: vscode.WebviewPanel } = { position, releasedFrom };
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

    private async dragPlayback(panel: vscode.WebviewPanel, phase: PlaybackDragPhase, position: string, releaseId?: string): Promise<void> {
        if (phase === "begin") {
            if (!panel.visible) return;
            this.finishPlaybackDrag();
            this.playbackDrag = { panel, dragId: crypto.randomBytes(8).readBigUInt64BE().toString(), position };
        }
        const drag = this.playbackDrag;
        if (phase === "end" && releaseId) this.dragReleaseIds.set(panel, releaseId);
        if (drag?.panel !== panel) {
            if (phase === "end" && releaseId) this.postPlayback();
            return;
        }
        drag.position = position;
        if (phase === "end") this.playbackDrag = null;
        try { await this.seekPlayback(position, { phase, dragId: drag.dragId }, phase === "end" ? panel : undefined); }
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
        const profile = this.playbackProfile?.recorder;
        if (this.disposed || this.playbackReading || (!profile && ![...this.ready].some((panel) => panel.visible))) return;
        const sent = profile ? performance.now() : 0;
        this.playbackReading = true;
        try {
            this.playbackSnapshot = await this.session.readPlayback(!!profile);
            if (!this.playbackSnapshot) this.pendingSeek = null;
        } catch {
            this.playbackSnapshot = null;
        } finally {
            this.playbackReading = false;
            if (profile && this.playbackProfile?.recorder === profile) {
                profile.recordPlayback(this.playbackSnapshot, sent, performance.now());
            }
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
        private readonly session: WorkspaceSession, private readonly outputChannel: vscode.OutputChannel,
        private readonly nativeHelper?: NativeClientHelper, private readonly pickerStorage?: vscode.Memento) {
        this.state.definitionUsage = pickerStorage?.get<Record<string, number>>("nodeDefinitionUsage") ?? {};
        this.changed = session.onDidChangeGraph(() => {
            if (session.graphSessionStatus !== "running") {
                this.observations.clear();
                this.panelObservations.clear();
            }
            void this.refresh();
        });
        const reloadPresentations = () => {
            this.presentationCatalog = undefined;
            for (const panel of this.ready) void this.postProjectPresentations(panel);
        };
        this.presentationChanges = vscode.Disposable.from(session.onDidChangePresentations(reloadPresentations),
            vscode.workspace.onDidGrantWorkspaceTrust(reloadPresentations));
        this.controlsChanged = session.onDidChangeManualControls(() => {
            for (const panel of this.ready) this.postControls(panel);
        });
        this.observationsChanged = session.onDidChangeObservations((update) => {
            if (session.graphSessionStatus !== "running") return;
            this.playbackProfile?.recorder.recordObservation(update, performance.now());
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

    async debug(): Promise<void> {
        const profileDurationMs = 10_000;
        const traceId = crypto.randomUUID();
        const actions: Array<vscode.QuickPickItem & { request: GraphEditorHostMessage; status: string; durationMs: number }> = [
            { label: "Trace Cursor Return", description: "Records the next playhead or control-point drag",
                request: { type: "startCursorReturnTrace", traceId },
                status: "Cursor return trace armed. Drag the playhead or a control point, then check Intravenous output.", durationMs: 30_000 },
            { label: "Profile Observation Updates", description: "Records lane requests and replies for 10 seconds",
                request: { type: "startObservationProfile", durationMs: profileDurationMs },
                status: "Recording observation updates for 10 seconds. See Intravenous output for the report.", durationMs: profileDurationMs },
            { label: "Record Client Interaction Trace", description: "Saves viewport, knob, and RPC activity for headless replay",
                request: { type: "startClientInteractionTrace", traceId, durationMs: profileDurationMs },
                status: "Recording client interactions for 10 seconds. Pan to reproduce the problem.", durationMs: profileDurationMs },
            { label: "Profile Project Graph UI", description: "Measures frame stalls, webview work, and server waits for 10 seconds",
                request: { type: "startGraphUiProfile", profileId: traceId, durationMs: profileDurationMs },
                status: "Recording project graph UI for 10 seconds. Reproduce the slowdown, then check Intravenous output.",
                durationMs: profileDurationMs },
            { label: "Profile Audio Playback", description: "Records audio samples at graph switches, render timing, and background underruns",
                request: { type: "startPlaybackProfile", profileId: traceId, durationMs: profileDurationMs },
                status: "Recording playback for 10 seconds. Pan and press M when you hear a click.",
                durationMs: profileDurationMs },
        ];
        const action = await vscode.window.showQuickPick(actions, { placeHolder: "Choose an Intravenous diagnostic" });
        if (!action) return;
        const panel = [...this.ready].find((entry) => entry.active)
            ?? (this.lastActivePanel && this.ready.has(this.lastActivePanel) ? this.lastActivePanel : null);
        if (!panel) {
            void vscode.window.showInformationMessage("Open the Intravenous Graph editor to run this diagnostic.");
            return;
        }
        panel.reveal(panel.viewColumn, false);
        if (action.request.type === "startPlaybackProfile" && this.playbackProfile) {
            void vscode.window.showInformationMessage("An Intravenous playback profile is already running.");
            return;
        }
        const profile = action.request.type === "startGraphUiProfile"
            ? { id: action.request.profileId, waits: new GraphUiTimings() } : undefined;
        if (profile) this.graphUiProfiles.set(panel, profile);
        try {
            if (action.request.type === "startClientInteractionTrace")
                this.startClientTrace(panel, action.request.traceId, action.request.durationMs);
            if (!await panel.webview.postMessage(action.request)) {
                if (action.request.type === "startClientInteractionTrace") this.stopClientTrace(panel);
                if (profile && this.graphUiProfiles.get(panel) === profile) this.graphUiProfiles.delete(panel);
                void vscode.window.showInformationMessage("The Intravenous Graph editor could not start the diagnostic.");
                return;
            }
            if (action.request.type === "startCursorReturnTrace") this.logCursorReturnTrace(action.request.traceId, action.status);
            else this.outputChannel.appendLine(action.status);
            if (action.request.type === "startPlaybackProfile") {
                this.startPlaybackProfile(panel, action.request.profileId, action.request.durationMs);
            }
            vscode.window.setStatusBarMessage(action.status, action.durationMs);
        } catch (error) {
            if (action.request.type === "startClientInteractionTrace") this.stopClientTrace(panel);
            if (profile && this.graphUiProfiles.get(panel) === profile) this.graphUiProfiles.delete(panel);
            void vscode.window.showErrorMessage(`Cannot start the Intravenous diagnostic: ${error}`);
        }
    }

    private startClientTrace(panel: vscode.WebviewPanel, id: string, durationMs: number): void {
        const owner = this.controlOwners.get(panel);
        if (!owner || this.session.graphSessionStatus !== "running") throw new Error("The project session is unavailable");
        this.stopClientTrace();
        const started = performance.now();
        const playback = this.playbackSnapshot;
        const trace: Omit<ClientInteractionTrace, keyof InteractionRecording> = { version: 1, durationMs, owner,
            view: { active: panel.active, visible: panel.visible }, expanded: this.expandedByPanel.get(panel) ?? [],
            playback: playback && { running: playback.running, position: playback.running
                ? playback.executionIndex ?? playback.position : playback.position }, sent: [] };
        const stop = this.session.observeRpcSends((value) => {
            const atMs = performance.now() - started;
            if (trace.sent.length < 8192 && isInteractionSend(value, owner))
                trace.sent.push({ atMs, value });
        });
        if (!stop) throw new Error("The project session is unavailable");
        this.clientTrace = { id, panel, started, stop, trace, events: [],
            timer: setTimeout(() => {
                this.stopClientTrace(panel);
                this.outputChannel.appendLine("Client interaction trace stopped before the webview returned its recording.");
            }, durationMs + 5000) };
    }

    private stopClientTrace(panel?: vscode.WebviewPanel) {
        const trace = this.clientTrace;
        if (!trace || (panel && trace.panel !== panel)) return;
        this.clientTrace = undefined;
        clearTimeout(trace.timer);
        trace.stop();
        return trace;
    }

    private recordClientView(panel: vscode.WebviewPanel, value: ClientInput): void {
        const trace = this.clientTrace;
        if (trace?.panel === panel && trace.events.length < 8192)
            trace.events.push({ atMs: performance.now() - trace.started, value });
    }

    private async saveClientTrace(panel: vscode.WebviewPanel, id: string, recording: InteractionRecording): Promise<void> {
        if (this.clientTrace?.id !== id) return;
        const capture = this.stopClientTrace(panel);
        if (!capture) return;
        try {
            const path = join(await mkdtemp(join(tmpdir(), "intravenous-client-trace-")), "trace.json");
            const offset = performance.timeOrigin + capture.started - recording.startedAtMs;
            const align = <T,>(entries: Timed<T>[]): Timed<T>[] => entries
                .map(({ atMs, value }) => ({ atMs: atMs + offset, value }))
                .filter(({ atMs }) => atMs >= 0 && atMs <= capture.trace.durationMs);
            const trace: ClientInteractionTrace = { ...capture.trace, ...recording,
                truncated: recording.truncated || capture.trace.sent.length >= 8192 || capture.events.length >= 8192,
                sent: align(capture.trace.sent),
                events: [...recording.events, ...align(capture.events)].sort((a, b) => a.atMs - b.atMs) };
            await writeFile(path, JSON.stringify(trace, null, 2) + "\n", { mode: 0o600 });
            this.outputChannel.appendLine(`Client interaction trace saved to ${path}`);
            this.notifyProfileFinished("client interaction trace");
        } catch (error) { void vscode.window.showErrorMessage(`Cannot save the client interaction trace: ${error}`); }
    }

    private startPlaybackProfile(panel: vscode.WebviewPanel, id: string, durationMs: number): void {
        if (this.disposed || this.playbackProfile) return;
        const now = performance.now();
        const recorder = new PlaybackProfile(now);
        recorder.recordGraph(this.state.scope?.execution, now);
        const outputStart = this.session.readPlayback(false, { id, durationMs })
            .then((snapshot) => snapshot?.outputProfile, () => undefined);
        const timer = setTimeout(() => {
            recorder.stop(performance.now());
            void this.finishPlaybackProfile(id, recorder, outputStart);
        }, durationMs);
        this.playbackProfile = { id, panel, recorder, timer };
        void this.refreshPlayback();
    }

    private async finishPlaybackProfile(id: string, recorder: PlaybackProfile,
        outputStart: Promise<PlaybackOutputProfileResult | undefined>): Promise<void> {
        let output = await outputStart ?? { status: "unavailable" };
        try {
            if (output.status === "recording") {
                for (let attempt = 0; attempt < 8 && !this.disposed; ++attempt) {
                    output = (await this.session.readPlayback(false, { id }))?.outputProfile ?? { status: "unavailable" };
                    if (output.status !== "pending") break;
                    await new Promise<void>((resolve) => setTimeout(resolve, 25));
                }
            }
        } catch {
            output = { status: "unavailable" };
        }
        if (this.disposed || this.playbackProfile?.recorder !== recorder) return;
        this.playbackProfile = undefined;
        this.outputChannel.appendLine(recorder.report(performance.now(), output));
        this.notifyProfileFinished("audio playback profile");
    }

    private logCursorReturnTrace(id: string, step: string, details?: TraceDetails): void {
        this.outputChannel.appendLine(`Cursor return trace [${id.slice(0, 8)}]. ${step}${details ? ` ${JSON.stringify(details)}` : ""}`);
    }

    private notifyProfileFinished(profile: string): void {
        const showOutput = "Show Output";
        void vscode.window.showInformationMessage(`Intravenous finished recording the ${profile}.`, showOutput).then((action) => {
            if (action === showOutput && !this.disposed) this.outputChannel.show(true);
        });
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
        const [scope, definitions] = await Promise.all([
            this.session.readGraphScope(this.expandedModules()), this.session.graphDefinitions(),
        ]);
        if (!this.panels.has(panel)) return;
        this.session.manualControls.reconcile(graphScopeWithChildren(scope));
        this.state = { ...this.state, scope, definitions, sessionStatus: this.session.graphSessionStatus };
        this.post(panel);
        if (action === "addNode" || action === "createPackage") {
            void panel.webview.postMessage({ type: "graphAction", action });
            return;
        }
        if (action === "deleteConnection") {
            const nodes = graphScopeWithChildren(scope).nodes;
            const label = (matcher: GraphScope["connections"][number]["connection"]["outputs"][number]) => {
                const node = nodes.find((candidate) => addressKey(candidate.address) === addressKey(matcher));
                const name = node ? graphScopeNodeLabel(node) : "Unavailable node";
                return `${name}.${matcher.port.name ?? matcher.port.index ?? "*"}`;
            };
            const selected = await vscode.window.showQuickPick(scope.connections.filter((entry) => !entry.sourceOwned && !isGeneratedSnapConnection(entry)).map((entry) => ({
                label: `${entry.connection.outputs.map(label).join(", ")} → ${entry.connection.inputs.map(label).join(", ")}`,
                description: entry.applied ? "" : "Not applied", id: entry.id,
            })), { title: "Intravenous: Delete Connection", placeHolder: "Select a connection" });
            if (selected) { await this.session.deleteGraphConnection(selected.id); await this.refresh(); }
            return;
        }
        if (action === "deleteNode") {
            const selected = await vscode.window.showQuickPick(scope.nodes.filter((node) => !isTimelineSnapNode(node)).map((node) => ({
                label: node.instance.displayName || node.instance.definitionId.split(".").pop() || node.instance.definitionId,
                description: node.instance.definitionId, instanceId: node.instance.instanceId,
            })), { title: "Intravenous: Delete Nodes", placeHolder: "Select nodes to delete", canPickMany: true });
            if (selected?.length && this.panels.has(panel)) {
                await this.session.deleteGraphNodes(selected.map((node) => node.instanceId));
                await this.refresh();
            }
            return;
        }
        const selected = await vscode.window.showQuickPick(scope.nodes.filter((node) => !isTimelineSnapNode(node)).map((node, index) => ({
            label: node.instance.displayName || node.instance.definitionId.split(".").pop() || node.instance.definitionId,
            description: node.instance.definitionId, instanceId: node.instance.instanceId,
            detail: `Lane ${index + 1}${node.instance.configurationSource ? ` · ${node.instance.configurationSource}` : ""}`,
        })), { title: `Intravenous: ${action === "editNode" ? "Edit Node" : "Open Node Source"}`,
            placeHolder: "Select a node" });
        if (!selected || !this.panels.has(panel)) return;
        if (action === "editNode") {
            const current = await this.session.readGraphScope(this.expandedModules());
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
            this.recordClientView(panel, { type: "view", visible: panel.visible, active: panel.active });
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
        let clipboardOperation = Promise.resolve();
        const messages = panel.webview.onDidReceiveMessage(async (message: GraphEditorWebviewMessage) => {
            if (message?.type === "observationDescribe" || message?.type === "observationReconcile") {
                const owner = this.controlOwners.get(panel);
                if (!owner) return;
                const read = () => answerObservationRequest(this.session, panel, owner, message);
                const waits = this.graphUiProfiles.get(panel)?.waits;
                const reply = await (waits ? waits.measureAsync(message.type === "observationDescribe"
                    ? "Describe observations" : "Reconcile observation interests", read) : read());
                if (this.panels.has(panel)) void panel.webview.postMessage(reply);
            } else if (message?.type === "clientInteractionTrace") {
                await this.saveClientTrace(panel, message.traceId, message.recording);
            } else if (message?.type === "observationResync") {
                this.postObservationSnapshot(panel);
            } else if (message?.type === "cursorReturnTrace") {
                this.logCursorReturnTrace(message.traceId, message.step, message.details);
            } else if (message?.type === "playbackProfileClick") {
                const profile = this.playbackProfile;
                if (profile?.panel === panel && message.profileId === profile.id) profile.recorder.markClick(performance.now());
            } else if (message?.type === "graphUiProfile") {
                const profile = this.graphUiProfiles.get(panel);
                if (!profile || profile.id !== message.profileId) return;
                this.graphUiProfiles.delete(panel);
                for (const line of graphUiProfileReport(message.summary, profile.waits.summary())) this.outputChannel.appendLine(line);
                this.notifyProfileFinished("project graph UI profile");
            } else if (message?.type === "observationProfile") {
                const s = message.summary;
                const updateRate = (s.updates / s.seconds).toFixed(2);
                const dataRate = (s.messageBytes / 1024 / s.seconds).toFixed(2);
                this.outputChannel.appendLine(`Observation profile over ${s.seconds.toFixed(2)} s`);
                this.outputChannel.appendLine(`  Webview received ${s.updates} updates (${updateRate}/s, about ${dataRate} KiB/s as JSON).`
                    + ` ${s.presentationUpdates} changed at least one stored observation.`);
                this.outputChannel.appendLine(`  ${s.snapshots} snapshots, ${s.metadataOnly} updates with only frame metadata,`
                    + ` ${s.liveValues} live readings, and ${s.removals} removals.`);
                this.outputChannel.appendLine(`  ${s.windowChanges} window updates (${s.statusChanges} included status),`
                    + ` ${s.windowBodies} included sample or event data (${(s.windowBytes / 1024).toFixed(2)} KiB).`);
                this.outputChannel.appendLine(`  Measurement took ${s.measurementMs.toFixed(2)} ms in the webview.`);
                for (const line of observationInterestReport(s)) this.outputChannel.appendLine(line);
                this.notifyProfileFinished("observation profile");
            } else if (isNodeEventRequest(message) || isNodeEventGestureRequest(message)) {
                const owner = this.controlOwners.get(panel);
                if (!owner) return;
                this.recordClientView(panel, { type: "nodeEvent", request: message });
                const response = await answerNodeEventRequest(this.session.nodeEvents, owner, message);
                if (this.panels.has(panel)) void panel.webview.postMessage(response);
                if (response.error) void vscode.window.showErrorMessage(`Cannot send node event: ${response.error}`);
            } else if (message?.type === "cursorReturnSkipped" && typeof message.detail === "string") {
                this.outputChannel.appendLine(`Cursor return skipped. ${message.detail}`);
            } else if (message?.type === "returnCursor") {
                const { traceId } = message;
                const trace = traceId ? (step: string, details?: TraceDetails) => this.logCursorReturnTrace(traceId, step, details) : undefined;
                trace?.("Extension received cursor return", { requestId: message.requestId, panelActive: panel.active,
                    windowFocused: vscode.window.state.focused, helperAvailable: !!this.nativeHelper, policy: message.policy });
                const skipped = !panel.active || !vscode.window.state.focused ? "graph editor lost focus"
                    : message.policy === "presentationPoint"
                        && !vscode.workspace.getConfiguration("intravenous").get("warpCursorAfterPointDrag", true) ? "cursor return is disabled"
                    : !this.nativeHelper ? "no local native helper" : null;
                if (skipped) {
                    trace?.("Extension skipped cursor return", { reason: skipped });
                    if (skipped === "graph editor lost focus") this.outputChannel.appendLine(`Cursor return skipped. ${skipped}.`);
                } else this.nativeHelper?.warpCursor(message.warp, {
                    hideCursor: message.policy === "wrapped" || message.policy === "playhead",
                    followPointer: message.policy === "wrapped",
                    completed: () => {
                        if (this.panels.has(panel)) void panel.webview.postMessage({ type: "cursorReturnCompleted", requestId: message.requestId });
                    },
                    trace,
                });
            } else if (message?.type === "playbackToggle" || message?.type === "playbackSeek" || message?.type === "playbackDrag") {
                try {
                    if (message.type === "playbackDrag") {
                        await this.dragPlayback(panel, message.phase, playbackSeekPosition(message.position, 1), message.releaseId);
                    } else if (message.type === "playbackSeek") await this.seekPlayback(playbackSeekPosition(message.position, 1));
                    else await this.session.playbackCommand();
                }
                catch (error) { void vscode.window.showErrorMessage(`Cannot change playback: ${error.message}`); }
            } else if (message?.type === "graphReady") {
                if (!this.panels.has(panel)) return;
                this.expandedByPanel.delete(panel);
                this.interactionHints.delete(panel);
                this.updateInteractionStatus();
                if (this.ready.has(panel)) {
                    this.finishPlaybackDrag(panel);
                    const owner = this.controlOwners.get(panel);
                    if (owner) { this.session.manualControls.releaseOwner(owner); this.session.nodeEvents.releaseOwner(owner); }
                }
                this.ready.add(panel);
                void this.postProjectPresentations(panel);
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
            } else if (message?.type === "duplicateSelectedLanes") {
                const { copies, requestId } = message;
                try {
                    await this.session.duplicateGraphNodes(copies);
                    void panel.webview.postMessage({ type: "graphLanesInserted", duplicateRequestId: requestId,
                        lanes: copies.map(({ instanceId }) => ({ instanceId })) });
                    await this.refresh();
                } catch (error) {
                    void panel.webview.postMessage({ type: "graphLanesInserted", duplicateRequestId: requestId, lanes: [] });
                    void vscode.window.showErrorMessage(`Cannot duplicate lanes: ${error.message}`);
                }
            } else if (message?.type === "setManualControlEnabled") {
                try { await this.session.setManualControlEnabled(message.control, message.enabled, this.expandedByPanel.get(panel) ?? []); }
                catch (error) { void vscode.window.showErrorMessage(`Cannot change knob: ${error.message}`); }
            } else if (message?.type === "projectHistory") {
                try {
                    await this.session.projectHistory(message.direction);
                    await this.refresh();
                } catch (error) { void vscode.window.showErrorMessage(`Cannot ${message.direction} project edit: ${error.message}`); }
            } else if (message?.type === "knobClipboard") {
                const { requestId, text } = message;
                const active = panel.active && vscode.window.state.focused;
                clipboardOperation = clipboardOperation.then(async () => {
                    const reply: GraphEditorHostMessage = { type: "knobClipboardResult", requestId };
                    try {
                        if (active) {
                            if (text === undefined) reply.text = await vscode.env.clipboard.readText();
                            else await vscode.env.clipboard.writeText(text);
                        }
                    } catch {
                        reply.error = text === undefined ? "Could not read the system clipboard." : "Could not copy to the system clipboard.";
                    }
                    if (this.panels.has(panel)) void panel.webview.postMessage(reply);
                });
            } else if (message?.type === "setExpandedModules" && Array.isArray(message.modules)) {
                this.recordClientView(panel, { type: "expanded", nodes: message.modules });
                this.expandedByPanel.set(panel, message.modules);
                await this.refresh();
            } else if (message?.type === "copyNodeDebugDetails") {
                const scope = this.state.scope && graphScopeWithChildren(this.state.scope);
                const node = scope?.nodes.find((entry) => addressKey(entry.address) === message.nodeKey);
                if (!scope || !node) return;
                try { await vscode.env.clipboard.writeText(nodeDebugDetails(scope, node)); }
                catch (error) { void vscode.window.showErrorMessage(`Cannot copy node details: ${error.message}`); }
            } else if (message?.type === "openVirtualNodeSource") {
                const scope = this.state.scope && graphScopeWithChildren(this.state.scope);
                const node = scope?.nodes.find((entry) => entry.virtualNode && addressKey(entry.address) === message.nodeKey);
                const source = node?.declarationSource;
                if (!source) return;
                try { await this.session.revealSourceFile(source.filePath, source); }
                catch (error) { void vscode.window.showErrorMessage(`Cannot open node source: ${error.message}`); }
            } else if (message?.type === "setTimelineSnapMode") {
                if (message.mode !== "seconds" && message.mode !== "beats" && message.mode !== "custom") return;
                try {
                    await this.session.setTimelineSnapMode(message.mode);
                    await this.refresh();
                } catch (error) { void vscode.window.showErrorMessage(`Cannot change timeline snapping: ${error.message}`); }
            } else if (message?.type === "connectGraphPorts" || message?.type === "editGraphConnectionEnd") {
                try {
                    const expanded = this.expandedByPanel.get(panel) ?? [];
                    if (message.type === "connectGraphPorts") {
                        await this.session.connectGraphPorts(crypto.randomUUID(), message.first, message.second, expanded);
                    } else await this.session.editGraphConnectionEnd(message.edit, expanded);
                    await this.refresh();
                } catch (error) {
                    void vscode.window.showErrorMessage(`Cannot edit connection: ${error.message}`);
                }
            } else if (message?.type === "searchNodeDefinitions") {
                const { requestId, source, cursor } = message;
                try {
                    const result = await this.session.searchNodeDefinitions(source, cursor);
                    void panel.webview.postMessage({ type: "nodeDefinitionSearchResult", requestId, result });
                } catch (error) {
                    void panel.webview.postMessage({ type: "nodeDefinitionSearchResult", requestId, error: error.message });
                }
            } else if (message?.type === "createGraphNode" || message?.type === "updateGraphNode"
                || message?.type === "renameGraphLane" || message?.type === "createGraphPackage") {
                try {
                    if (message.type === "createGraphNode") {
                        const instanceId = await this.session.createGraphNode(message.definitionId, message.displayName,
                            message.configurationSource, message.afterInstanceId);
                        const usage = this.state.definitionUsage;
                        this.state.definitionUsage = { ...usage, [message.definitionId]:
                            (Object.hasOwn(usage, message.definitionId) ? usage[message.definitionId] : 0) + 1 };
                        void this.pickerStorage?.update("nodeDefinitionUsage", this.state.definitionUsage).then(undefined,
                            (error) => this.outputChannel.appendLine(`Cannot save node picker usage: ${error.message}`));
                        void panel.webview.postMessage({ type: "graphLanesInserted",
                            lanes: [{ instanceId }] });
                    } else if (message.type === "updateGraphNode") {
                        await this.session.updateGraphNode(message.instanceId, message.displayName, message.previousDisplayName,
                            { source: message.configurationSource, previous: message.previousConfigurationSource });
                    } else if (message.type === "renameGraphLane") {
                        await this.session.updateGraphNode(message.instanceId, message.displayName, message.previousDisplayName);
                    } else {
                        await this.session.createGraphPackage(message.name);
                    }
                    await this.refresh();
                } catch (error) {
                    void vscode.window.showErrorMessage(`Cannot edit project: ${error.message}`);
                }
            } else if (message?.type === "saveGraphPresentation") {
                const { requestId, presentationStates, laneOrder, laneParents } = message;
                try {
                    const result = await this.session.saveGraphPresentation({ presentationStates, laneOrder, laneParents });
                    void panel.webview.postMessage({ type: "graphPresentationAccepted", requestId, revision: result.sessionRevision });
                    await this.refresh();
                } catch (error) {
                    void panel.webview.postMessage({ type: "graphPresentationAccepted", requestId });
                    void vscode.window.showErrorMessage(`Cannot save lane presentation: ${error.message}`);
                }
            } else if (message?.type === "flushProjectAutosave") {
                try { await this.session.saveProject(true); }
                catch (error) { this.outputChannel.appendLine(`Project autosave failed: ${error.message}`); }
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
            this.stopClientTrace(panel);
            this.graphUiProfiles.delete(panel);
            this.expandedByPanel.delete(panel);
            this.updateInteractionStatus();
            this.pendingActions.delete(panel);
            if (this.lastActivePanel === panel) this.lastActivePanel = null;
        });
        const nonce = crypto.randomBytes(16).toString("base64");
        const script = panel.webview.asWebviewUri(vscode.Uri.joinPath(root, "main.js"));
        const css = panel.webview.asWebviewUri(vscode.Uri.joinPath(root, "main.css"));
        panel.webview.html = `<!doctype html><html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src ${panel.webview.cspSource} data:; style-src ${panel.webview.cspSource} 'unsafe-inline'; script-src 'nonce-${nonce}' 'wasm-unsafe-eval'; worker-src blob:">
<link rel="stylesheet" href="${css}"></head><body data-surface="graphEditor"><div id="app"></div>
<script type="module" nonce="${nonce}" src="${script}"></script></body></html>`;
    }

    private async postProjectPresentations(panel: vscode.WebviewPanel): Promise<void> {
        const catalog = this.presentationCatalog ??= this.readPresentationCatalog();
        try {
            const plugins = await catalog;
            if (this.ready.has(panel) && catalog === this.presentationCatalog)
                void panel.webview.postMessage({ type: "projectPresentations", plugins } satisfies GraphEditorHostMessage);
        } catch (error) {
            if (this.presentationCatalog !== catalog) return;
            this.presentationCatalog = undefined;
            this.outputChannel.appendLine(`Presentation catalog · ${String(error)}`);
        }
    }

    private async readPresentationCatalog(): Promise<PresentationArtifact[]> {
        if (!vscode.workspace.isTrusted || this.session.graphSessionStatus !== "running") return [];
        const rpc = this.session.presentationArtifactRpc();
        return rpc ? (await this.presentationReader.read(rpc, (error) => this.outputChannel.appendLine(error))).artifacts : [];
    }

    private postControls(panel: vscode.WebviewPanel): void {
        const owner = this.controlOwners.get(panel);
        if (owner && this.ready.has(panel)) void panel.webview.postMessage({
            type: "manualControlState", interactions: this.session.manualControls.states(owner),
            live: this.session.manualControls.live(owner),
        });
    }

    private post(panel: vscode.WebviewPanel): void {
        if (!this.ready.has(panel)) return;
        const waits = this.graphUiProfiles.get(panel)?.waits;
        const message: GraphEditorHostMessage = { type: "graphState", state: this.state };
        if (!waits) void panel.webview.postMessage(message);
        else void waits.measureAsync("Post graph state to webview", () => panel.webview.postMessage(message));
    }

    private expandedModules(): NodeAddress[] {
        const addresses = [...this.expandedByPanel.values()].flat();
        return [...new Map(addresses.map((address) => [addressKey(address), address])).values()];
    }

    private measureGraphRead<T>(label: string, read: () => Promise<T>): Promise<T> {
        if (!this.graphUiProfiles.size) return read();
        const profiles = [...this.graphUiProfiles.values()];
        const start = performance.now();
        return read().finally(() => {
            const elapsed = performance.now() - start;
            for (const profile of profiles) profile.waits.record(label, elapsed);
        });
    }

    private async refresh(): Promise<void> {
        if (!this.panels.size || this.disposed) return;
        this.dirty = true;
        if (this.reading) return;
        this.reading = true;
        try {
            while (this.dirty && !this.disposed) {
                this.dirty = false;
                this.state.sessionStatus = this.session.graphSessionStatus;
                if (this.state.sessionStatus !== "running") {
                    this.state.scope = null;
                    this.state.definitions = null;
                } else {
                    try {
                        const [scope, definitions] = await Promise.all([
                            this.measureGraphRead("Read graph scope", () => this.session.readGraphScope(this.expandedModules())),
                            this.measureGraphRead("Read node definitions", () => this.session.graphDefinitions()),
                        ]);
                        // A change while reading requires another complete read.
                        // Do not show a stale response, especially after restart.
                        if (this.dirty || this.disposed) continue;
                        this.session.manualControls.reconcile(graphScopeWithChildren(scope));
                        this.state.scope = scope;
                        this.playbackProfile?.recorder.recordGraph(scope.execution, performance.now());
                        this.state.definitions = definitions;
                        this.state.error = "";
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
        this.stopClientTrace();
        this.disposed = true;
        this.presentationChanges.dispose();
        clearInterval(this.playbackTimer);
        if (this.playbackProfile) clearTimeout(this.playbackProfile.timer);
        this.playbackProfile = undefined;
        this.changed.dispose();
        this.controlsChanged.dispose();
        this.observationsChanged.dispose();
        this.windowFocusChanged.dispose();
        this.shortcutSettingChanged.dispose();
        for (const panel of this.panels) panel.dispose();
        this.panels.clear();
        this.graphUiProfiles.clear();
        this.interactionStatus.dispose();
    }
}
