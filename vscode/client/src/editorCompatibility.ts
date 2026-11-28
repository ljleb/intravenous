// VS Code #329240 adds registered auxiliary windows to both permission handlers.
// Verified in the 1.133.0 release, not just in the current development branch:
// https://github.com/microsoft/vscode/blob/1.133.0/src/vs/code/electron-main/app.ts
const detachedPointerLockFixedVersion = [1, 133, 0];

export function knobPointerLockVersionWarning(editorName: string, codeVersion: string): string | null {
    const match = /^(\d+)\.(\d+)\.(\d+)(?:-([\w.-]+))?(?:\+[\w.-]+)?$/.exec(codeVersion);
    // Custom builds can use non-version labels. Their capability is unknown.
    if (!match) return null;
    const version = match.slice(1, 4).map(Number);
    let comparison = 0;
    for (let i = 0; i < version.length; ++i) {
        comparison = version[i] - detachedPointerLockFixedVersion[i];
        if (comparison !== 0) break;
    }
    if (comparison > 0 || comparison === 0 && !match[4]) return null;
    return `Intravenous: ${editorName} uses VS Code ${codeVersion}. Older editor versions can reject knob pointer lock in detached windows. `
        + "Update to an editor based on VS Code 1.133.0 or newer, or keep the graph in the main editor window.";
}
