export type ControlCompileStatus = {
    requestId: number;
    projectGeneration: number;
    phase: "queued" | "compiling" | "compilingStorage" | "prepared" | "staged" | "superseded" | "failed";
    dynamic: boolean;
    predictive: boolean;
    inputs: string[];
    elapsedUs: number;
    queueUs: number;
    preparationUs: number;
    stageUs: number;
    message: string;
};

const milliseconds = (us: number) => `${(us / 1000).toFixed(2)} ms`;

// One compiler request can be prepared by hover or release and staged later. Keep
// preparation and staging times separate; neither proves audio activation.
export class ControlCompilationProgress {
    private readonly active = new Map<number, "queued" | "compiling">();
    clear(): void { this.active.clear(); }
    receive(status: ControlCompileStatus) {
        if (status.phase === "queued") this.active.set(status.requestId, "queued");
        else if (status.phase === "compiling" || status.phase === "compilingStorage")
            this.active.set(status.requestId, "compiling");
        else this.active.delete(status.requestId);
        const queued = [...this.active.values()].filter((phase) => phase === "queued").length;
        const compiling = this.active.size - queued;
        const label = `${status.dynamic ? "dynamic" : "settled"}${status.inputs.length ? ` (${status.inputs.join(", ")})` : ""}`;
        const prefix = `Intravenous control compile #${status.requestId}, generation ${status.projectGeneration}: ${label}`;
        let result: string;
        switch (status.phase) {
            case "queued": result = `queued${status.predictive ? " for predictive preparation" : ""}`; break;
            case "compiling": result = `compiling; queue ${milliseconds(status.queueUs)}`; break;
            case "compilingStorage": result = "compiling storage transition"; break;
            case "prepared": result = `prepared; request-to-ready ${milliseconds(status.preparationUs)} (queue ${milliseconds(status.queueUs)})`; break;
            case "staged": result = `executor accepted; staging ${milliseconds(status.stageUs)}, request-to-staging ${milliseconds(status.elapsedUs)}`; break;
            case "superseded": result = `superseded after ${milliseconds(status.elapsedUs)}`; break;
            case "failed": result = `failed after ${milliseconds(status.elapsedUs)}`; break;
        }
        const text = `${prefix}: ${result}${status.message ? `; ${status.message}` : ""}`;
        return { text, queued, compiling, failed: status.phase === "failed",
            summary: status.phase === "staged" ? `Controls staged ${milliseconds(status.stageUs)}`
                : status.phase === "prepared" ? `Controls prepared ${milliseconds(status.preparationUs)}`
                    : status.phase === "failed" ? "Control compilation failed" : "Control compilation superseded" };
    }
}
