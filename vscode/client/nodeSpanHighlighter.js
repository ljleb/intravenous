const vscode = require("vscode");

class NodeSpanHighlighter {
    constructor() {
        this.spans = [];
        this.activeRegions = [];
        this.decorationType = vscode.window.createTextEditorDecorationType({
            backgroundColor: "rgba(118, 173, 255, 0.14)",
            borderColor: "rgba(118, 173, 255, 0.60)",
            borderStyle: "solid",
            borderWidth: "1px",
            rangeBehavior: vscode.DecorationRangeBehavior.ClosedClosed,
            overviewRulerColor: "rgba(118, 173, 255, 0.75)",
            overviewRulerLane: vscode.OverviewRulerLane.Right,
        });
        this.regionDecorationType = vscode.window.createTextEditorDecorationType({
            backgroundColor: "rgba(160, 190, 255, 0.045)",
            borderColor: "rgba(160, 190, 255, 0.10)",
            borderStyle: "solid",
            borderWidth: "1px",
            rangeBehavior: vscode.DecorationRangeBehavior.ClosedClosed,
        });
    }

    dispose() {
        this.clear();
        this.decorationType.dispose();
        this.regionDecorationType.dispose();
    }

    clear() {
        this.spans = [];
        this.activeRegions = [];
        for (const editor of vscode.window.visibleTextEditors) {
            editor.setDecorations(this.decorationType, []);
            editor.setDecorations(this.regionDecorationType, []);
        }
    }

    clearPrimary() {
        this.spans = [];
        this.refresh();
    }

    setSpans(spans) {
        this.spans = Array.isArray(spans) ? spans : [];
        this.refresh();
    }

    setActiveRegions(spans) {
        this.activeRegions = Array.isArray(spans) ? spans : [];
        this.refresh();
    }

    refresh() {
        for (const editor of vscode.window.visibleTextEditors) {
            this.applyToEditor(editor);
        }
    }

    applyToEditor(editor) {
        if (editor.document.uri.scheme !== "file") {
            editor.setDecorations(this.decorationType, []);
            editor.setDecorations(this.regionDecorationType, []);
            return;
        }

        const filePath = editor.document.uri.fsPath;
        const decorations = this.spans
            .filter((span) => span.filePath === filePath)
            .map((span) => new vscode.Range(
                Math.max(span.start.line - 1, 0),
                Math.max(span.start.column - 1, 0),
                Math.max(span.end.line - 1, 0),
                Math.max(span.end.column - 1, 0)
            ));
        const primaryKeys = new Set(decorations.map((range) => `${range.start.line}:${range.start.character}:${range.end.line}:${range.end.character}`));
        const regionDecorations = this.activeRegions
            .filter((span) => span.filePath === filePath)
            .map((span) => new vscode.Range(
                Math.max(span.start.line - 1, 0),
                Math.max(span.start.column - 1, 0),
                Math.max(span.end.line - 1, 0),
                Math.max(span.end.column - 1, 0)
            ))
            .filter((range) => !primaryKeys.has(`${range.start.line}:${range.start.character}:${range.end.line}:${range.end.character}`));
        editor.setDecorations(this.decorationType, decorations);
        editor.setDecorations(this.regionDecorationType, regionDecorations);
    }
}

module.exports = { NodeSpanHighlighter };
