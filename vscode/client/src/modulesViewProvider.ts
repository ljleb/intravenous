import * as vscode from "vscode";

export type ModuleSourceInfo = {
    moduleId: string;
    moduleRoot: string;
    projectLocal: boolean;
};

export type ModuleInstanceInfo = {
    instanceId: string;
    definitionId: string;
    displayName: string;
    moduleId?: string;
    moduleRoot: string;
    realized: boolean;
};

export type ModulesControlMessage =
    | { type: "createSource"; name: string }
    | { type: "instantiate"; moduleRoot: string }
    | { type: "open"; instanceId: string; moduleRoot: string }
    | { type: "rename"; instanceId: string; displayName: string }
    | { type: "duplicate"; moduleRoot: string }
    | { type: "delete"; instanceId: string }
    | { type: "reveal"; moduleRoot: string }
    | { type: "select"; instanceId: string };

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
        case "open": return typeof message.instanceId === "string" && typeof message.moduleRoot === "string";
        case "rename": return typeof message.instanceId === "string" && typeof message.displayName === "string";
        case "duplicate": return typeof message.moduleRoot === "string";
        case "delete": return typeof message.instanceId === "string";
        case "select": return typeof message.instanceId === "string";
        default: return false;
        }
    }

    private getHtml(): string {
        const nonce = `${Date.now()}-${Math.random().toString(16).slice(2)}`;
        return `<!doctype html>
<html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'nonce-${nonce}'">
<style>
body{margin:0;color:var(--vscode-foreground);background:var(--vscode-editor-background);font-family:var(--vscode-font-family);font-size:var(--vscode-font-size)}
main{max-width:960px;margin:auto;padding:16px}.top,.rowhead,.rowmeta,.actions{display:flex;align-items:center;gap:8px}.top{justify-content:space-between;margin-bottom:16px}h1,h2,p{margin:0}h1{font-size:1.25em}h2{font-size:1em;color:var(--vscode-descriptionForeground);margin-bottom:6px}.section{margin-bottom:18px}.new-source{display:flex;gap:8px;margin-bottom:8px}input{flex:1;min-width:0}
button,input{font:inherit;padding:4px 8px;border:1px solid var(--vscode-input-border,transparent);color:var(--vscode-input-foreground);background:var(--vscode-input-background)}button{background:var(--vscode-button-secondaryBackground);cursor:pointer}button.primary{background:var(--vscode-button-background);color:var(--vscode-button-foreground)}
button.icon-button{width:28px;height:28px;padding:4px;display:inline-flex;align-items:center;justify-content:center}button.icon-button svg{width:18px;height:18px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round;pointer-events:none}button.danger{color:var(--vscode-errorForeground)}
.card{border:1px solid var(--vscode-widget-border);padding:8px 10px;margin:5px 0;overflow:hidden}.card.selected{border-color:var(--vscode-focusBorder);box-shadow:inset 2px 0 var(--vscode-focusBorder)}.rowhead{justify-content:space-between;min-width:0}.title{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-weight:600}.title.editable{cursor:text}.title-input{flex:1;min-width:0;font-weight:600;padding:2px 6px;border:1px solid var(--vscode-focusBorder);background:var(--vscode-input-background);color:var(--vscode-input-foreground)}.subtle{color:var(--vscode-descriptionForeground);font-size:.9em;white-space:nowrap}.rowmeta{justify-content:space-between;min-width:0;margin-top:3px}.meta{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--vscode-descriptionForeground);font-family:var(--vscode-editor-font-family);font-size:.85em}.actions{margin-top:6px;flex-wrap:wrap}.status{min-width:0;flex:0 0 auto;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:var(--vscode-descriptionForeground);font-size:.9em}.empty{color:var(--vscode-descriptionForeground);padding:12px 0}
</style></head><body><main>
<div class="top"><h1>Modules</h1><span id="summary" class="status"></span></div>
<section class="section"><h2>Sources</h2><div class="new-source"><input id="source-name" placeholder="new module name" autocomplete="off"><button class="primary icon-button" id="create-source" title="Create local source from template" aria-label="Create local source from template"><svg viewBox="0 0 18 18"><path d="M9 2v14M2 9h14M4 4l10 10M14 4 4 14"/></svg></button></div><div id="sources"></div></section>
<section class="section"><h2>Project instances</h2><div id="instances"></div></section>
</main><script nonce="${nonce}">
const vscode=acquireVsCodeApi();let state={sources:[],instances:[],selectedInstanceId:null};let renamingInstanceId=null;let renamingDraft='';
const byId=id=>document.getElementById(id);
const icons={add:'<svg viewBox="0 0 18 18"><path d="M9 3v12M3 9h12"/></svg>',code:'<svg viewBox="0 0 18 18"><path d="m6.5 3-5 6 5 6M11.5 3l5 6-5 6"/></svg>',copy:'<svg viewBox="0 0 18 18"><rect x="6" y="6" width="8" height="9" rx="1"/><path d="M12 6V4a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v8a1 1 0 0 0 1 1h1"/></svg>',trash:'<svg viewBox="0 0 18 18"><path d="M3 5h12M7 2h4M5 5l.7 10h6.6L13 5M7.5 8v4M10.5 8v4"/></svg>',reveal:'<svg viewBox="0 0 18 18"><path d="M3 3h5l2 2h5v10H3zM3 7h12"/></svg>'};
const button=(icon,label,cls,fn)=>{const b=document.createElement('button');b.className='icon-button'+(cls?' '+cls:'');b.innerHTML=icons[icon];b.title=label;b.setAttribute('aria-label',label);b.onclick=event=>{event.stopPropagation();fn()};return b};
const action=(type,fields)=>vscode.postMessage({type,...fields});const displayNameFor=instance=>instance.displayName||instance.moduleId||instance.definitionId;
const beginRename=instance=>{renamingInstanceId=instance.instanceId;renamingDraft=displayNameFor(instance);render()};const cancelRename=instanceId=>{if(renamingInstanceId!==instanceId)return;renamingInstanceId=null;renamingDraft='';render()};const commitRename=instance=>{if(renamingInstanceId!==instance.instanceId)return;const next=renamingDraft.trim();renamingInstanceId=null;renamingDraft='';render();action('rename',{instanceId:instance.instanceId,displayName:next})};
const sourceCard=source=>{const card=document.createElement('div');card.className='card';const head=document.createElement('div');head.className='rowhead';const title=document.createElement('div');title.className='title';title.textContent=source.moduleId;const kind=document.createElement('div');kind.className='subtle';kind.textContent=source.projectLocal?'Local':'Shared';head.append(title,kind);const meta=document.createElement('div');meta.className='meta';meta.textContent=source.moduleRoot;const actions=document.createElement('div');actions.className='actions';actions.append(button('add','Instantiate module','primary',()=>action('instantiate',{moduleRoot:source.moduleRoot})),button('code','Open module source','',()=>action('reveal',{moduleRoot:source.moduleRoot})));card.append(head,meta,actions);return card};
const instanceCard=instance=>{const card=document.createElement('div');card.className='card'+(instance.instanceId===state.selectedInstanceId?' selected':'');card.title='Select module instance';card.onclick=()=>action('select',{instanceId:instance.instanceId});const head=document.createElement('div');head.className='rowhead';if(renamingInstanceId===instance.instanceId){const title=document.createElement('input');title.className='title-input';title.value=renamingDraft;title.setAttribute('aria-label','Display name');title.oninput=()=>{renamingDraft=title.value};title.onblur=()=>commitRename(instance);title.onkeydown=event=>{if(event.key==='Enter'){event.preventDefault();commitRename(instance)}else if(event.key==='Escape'){event.preventDefault();cancelRename(instance.instanceId)}};title.onclick=event=>event.stopPropagation();head.append(title);setTimeout(()=>{title.focus();title.select()},0)}else{const title=document.createElement('div');title.className='title editable';title.textContent=displayNameFor(instance);title.title='Click to rename';title.onclick=event=>{event.stopPropagation();beginRename(instance)};head.append(title)}const status=document.createElement('span');status.className='status';status.textContent=instance.realized?'Realized':'Waiting';head.append(status);const metaRow=document.createElement('div');metaRow.className='rowmeta';const meta=document.createElement('div');meta.className='meta';meta.textContent=(instance.moduleId||instance.definitionId)+' · '+instance.instanceId;const source=document.createElement('div');source.className='meta';source.textContent=instance.moduleRoot;metaRow.append(meta,source);const actions=document.createElement('div');actions.className='actions';actions.append(button('code','Open module source','primary',()=>action('open',{instanceId:instance.instanceId,moduleRoot:instance.moduleRoot})),button('copy','Duplicate module','',()=>action('duplicate',{moduleRoot:instance.moduleRoot})),button('trash','Delete module instance','danger',()=>action('delete',{instanceId:instance.instanceId})));card.append(head,metaRow,actions);return card};
const render=()=>{byId('summary').textContent=state.sources.length+' sources · '+state.instances.length+' instances';const sources=byId('sources');sources.replaceChildren(...(state.sources.length?state.sources.map(sourceCard):[Object.assign(document.createElement('div'),{className:'empty',textContent:'No module sources discovered.'})]));const instances=byId('instances');instances.replaceChildren(...(state.instances.length?state.instances.map(instanceCard):[Object.assign(document.createElement('div'),{className:'empty',textContent:'No project instances yet.'})]));};
byId('create-source').onclick=()=>{const input=byId('source-name');const name=input.value.trim();if(!name)return;action('createSource',{name});input.value=''};window.addEventListener('message',event=>{if(event.data?.type==='setState'){state=event.data;if(renamingInstanceId&&!state.instances.some(instance=>instance.instanceId===renamingInstanceId)){renamingInstanceId=null;renamingDraft=''}render()}});render();
</script></body></html>`;
    }
}
