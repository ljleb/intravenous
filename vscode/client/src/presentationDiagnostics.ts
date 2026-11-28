import type { NodeAddress } from "./graphScope";
import type { PresentationArtifact } from "./presentationPlugin";
import { PresentationWorkerError } from "./wasmPresentationWorker";

export function presentationErrorDetails(context: {
    surface: "lane" | "beats-settings";
    operation: "frame" | "reload" | "restore" | "edit";
    address?: NodeAddress;
    definitionId?: string;
    artifact?: PresentationArtifact;
}, error: unknown): string {
    const { artifact, ...location } = context;
    return JSON.stringify({ ...location,
        artifact: artifact && { id: artifact.id, digest: artifact.digest },
        message: String(error).slice(0, 512),
        frame: error instanceof PresentationWorkerError ? error.frame : undefined,
        stack: error instanceof Error ? error.stack?.slice(0, 2500) : undefined,
    });
}
