import { inject, onScopeDispose, type InjectionKey } from "vue";
import { PresentationFrames } from "../../src/presentationFrames";

export const presentationFramesKey: InjectionKey<PresentationFrames> = Symbol("presentation frames");
export const createPresentationFrames = () => new PresentationFrames((run) => requestAnimationFrame(run),
    (id) => cancelAnimationFrame(id), (error) => globalThis.reportError(error));
let fallback: PresentationFrames | undefined;

export function usePresentationFrame(phase: "measure" | "update", run: (time: number) => void) {
    const frames = inject(presentationFramesKey, null) ?? (fallback ??= createPresentationFrames());
    const cancel = () => frames.cancel(run);
    onScopeDispose(cancel);
    return { schedule: () => frames.schedule(phase, run), cancel };
}
