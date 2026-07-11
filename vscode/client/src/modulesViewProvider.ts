import * as vscode from "vscode";

export type ModuleSourceInfo = {
    moduleId: string;
    moduleRoot: string;
    projectLocal: boolean;
};

export type ModuleInstanceInfo = {
    instanceId: string;
    definitionId: string;
    moduleId?: string;
    moduleRoot: string;
    realized: boolean;
};

export type ModulesControlMessage =
    | { type: "createSource"; name: string }
    | { type: "instantiate"; moduleRoot: string }
    | { type: "select"; instanceId: string; moduleRoot: string }
    | { type: "duplicate"; moduleRoot: string }
    | { type: "delete"; instanceId: string }
    | { type: "reveal"; moduleRoot: string };

export type ModulesControlHandler = (message: ModulesControlMessage) => Promise<void>;

export class ModulesViewProvider {
    private panel: vscode.WebviewPanel | null = null;
    private sources: ModuleSourceInfo[] = [];
    private instances: ModuleInstanceInfo[] = [];
    private selectedInstanceId: string | null = null;
    private controlHandler: ModulesControlHandler | null = null;

    open(): void {
        if (this.panel) {
            this.panel.reveal(vscode.ViewColumn.Beside);
            this.postState();
            return;
        }
        const panel = vscode.window.createWebviewPanel(
            "intravenous.modules",
            "Intravenous Modules",
            vscode.ViewColumn.Beside,
            { enableScripts: true, retainContextWhenHidden: true },
        );
        this.attachPanel(panel);
    }

    revive(panel: vscode.WebviewPanel): void {
        this.attachPanel(panel);
    }

    private attachPanel(panel: vscode.WebviewPanel): void {
        this.panel = panel;
        panel.onDidDispose(() => { this.panel = null; });
        panel.webview.onDidReceiveMessage(async (message: unknown) => {
            if (!this.isControlMessage(message) || !this.controlHandler) return;
            await this.controlHandler(message);
        });
        panel.webview.html = this.getHtml();
        this.postState();
    }

    setControlHandler(handler: ModulesControlHandler): void {
        this.controlHandler = handler;
    }

    setState(
        sources: ModuleSourceInfo[],
        instances: ModuleInstanceInfo[],
        selectedInstanceId: string | null,
    ): void {
        this.sources = sources;
        this.instances = instances;
        this.selectedInstanceId = selectedInstanceId;
        this.postState();
    }

    private postState(): void {
        void this.panel?.webview.postMessage({
            type: "setState",
            sources: this.sources,
            instances: this.instances,
            selectedInstanceId: this.selectedInstanceId,
        });
    }

    private isControlMessage(value: unknown): value is ModulesControlMessage {
        if (!value || typeof value !== "object") return false;
        const message = value as Record<string, unknown>;
        switch (message.type) {
        case "createSource": return typeof message.name === "string";
        case "instantiate":
        case "reveal": return typeof message.moduleRoot === "string";
        case "select": return typeof message.instanceId === "string" && typeof message.moduleRoot === "string";
        case "duplicate": return typeof message.moduleRoot === "string";
        case "delete": return typeof message.instanceId === "string";
        default: return false;
        }
    }

    private getHtml(): string {
        const nonce = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
        return `<!doctype html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}'"><style>
body{margin:0;color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);font-size:var(--vscode-font-size)}main{max-width:960px;margin:auto;padding:20px}.top,header,.actions{display:flex;align-items:center;gap:8px}.top{justify-content:space-between;margin-bottom:24px}h1,h2,p{margin:0}h1{font-size:1.4em}h2{font-size:1.05em;color:var(--vscode-descriptionForeground);margin-bottom:8px}.section{margin-bottom:28px}.new-source{display:flex;gap:8px;margin-bottom:10px}input{flex:1;min-width:0}button,input{font:inherit;padding:5px 8px;border:1px solid var(--vscode-input-border,transparent);color:var(--vscode-input-foreground);background:var(--vscode-input-background)}button{background:var(--vscode-button-secondaryBackground);cursor:pointer}button.primary{background:var(--vscode-button-background);color:var(--vscode-button-foreground)}button.danger{color:var(--vscode-errorForeground)}.card{border:1px solid var(--vscode-widget-border);padding:11px 12px;margin:7px 0;overflow:hidden}.card.selected{border-color:var(--vscode-focusBorder);box-shadow:inset 3px 0 var(--vscode-focusBorder)}header{min-width:0}.title{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-weight:600}.meta{color:var(--vscode-descriptionForeground);font-family:var(--vscode-editor-font-family);font-size:.9em;margin-top:3px;word-break:break-all}.actions{margin-top:9px;flex-wrap:wrap}.status{min-width:0;flex:0 1 auto;margin-left:auto;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--vscode-descriptionForeground)}.empty{color:var(--vscode-descriptionForeground);padding:12px 0}</style></head><body><main><div class="top"><h1>Modules</h1><span id="summary" class="status"></span></div><section class="section"><h2>Sources</h2><div class="new-source"><input id="source-name" placeholder="new module name" autocomplete="off"><button class="primary" id="create-source">Create local source</button></div><div id="sources"></div></section><section class="section"><h2>Project instances</h2><div id="instances"></div></section></main><script nonce="${nonce}">const vscode=acquireVsCodeApi();let state={sources:[],instances:[],selectedInstanceId:null};const byId=id=>document.getElementById(id);const button=(text,cls,fn)=>{const b=document.createElement('button');b.textContent=text;if(cls)b.className=cls;b.onclick=fn;return b};const action=(type,fields)=>vscode.postMessage({type,...fields});const sourceCard=source=>{const card=document.createElement('div');card.className='card';const title=document.createElement('div');title.className='title';title.textContent=source.moduleId;const meta=document.createElement('div');meta.className='meta';meta.textContent=(source.projectLocal?'Project local':'Shared source')+' · '+source.moduleRoot;const actions=document.createElement('div');actions.className='actions';actions.append(button('Instantiate','primary',()=>action('instantiate',{moduleRoot:source.moduleRoot})),button('Reveal source','',()=>action('reveal',{moduleRoot:source.moduleRoot})));card.append(title,meta,actions);return card};const instanceCard=instance=>{const card=document.createElement('div');card.className='card'+(instance.instanceId===state.selectedInstanceId?' selected':'');const header=document.createElement('header');const title=document.createElement('div');title.className='title';title.textContent=instance.moduleId||instance.definitionId;const status=document.createElement('span');status.className='status';status.textContent=instance.realized?'Realized':'Waiting to build';header.append(title,status);const meta=document.createElement('div');meta.className='meta';meta.textContent=instance.moduleRoot+' · '+instance.instanceId;const actions=document.createElement('div');actions.className='actions';actions.append(button('Select','primary',()=>action('select',{instanceId:instance.instanceId,moduleRoot:instance.moduleRoot})),button('Duplicate','',()=>action('duplicate',{moduleRoot:instance.moduleRoot})),button('Delete','danger',()=>action('delete',{instanceId:instance.instanceId})));card.append(header,meta,actions);return card};const render=()=>{byId('summary').textContent=state.sources.length+' sources · '+state.instances.length+' instances';const sources=byId('sources');sources.replaceChildren(...(state.sources.length?state.sources.map(sourceCard):[Object.assign(document.createElement('div'),{className:'empty',textContent:'No module sources discovered.'})]));const instances=byId('instances');instances.replaceChildren(...(state.instances.length?state.instances.map(instanceCard):[Object.assign(document.createElement('div'),{className:'empty',textContent:'No project instances yet.'})]));};byId('create-source').onclick=()=>{const input=byId('source-name');const name=input.value.trim();if(!name)return;action('createSource',{name});input.value=''};window.addEventListener('message',event=>{if(event.data?.type==='setState'){state=event.data;render()}});render();</script></body></html>`;
    }
}
