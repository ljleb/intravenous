import * as fs from "node:fs/promises";
import * as path from "node:path";
import { randomUUID } from "node:crypto";
import { isDeepStrictEqual } from "node:util";
import { readPresentationStates, readGraphViewState, readLayout, type PresentationState,
    type GraphLayout, type GraphViewState } from "./graphScope";

// Frontend-owned data, separate from iv_project.jsonl. Serialize saves and
// replace the complete file so a reload cannot read half a layout.
export class GraphLayoutStore {
    private pending: Promise<GraphViewState> = Promise.resolve(readGraphViewState(null));
    readonly filePath: string;

    constructor(projectDirectory: string) {
        this.filePath = path.join(projectDirectory, ".intravenous", "graph-layout.json");
    }

    async load(): Promise<GraphLayout> {
        try {
            return readLayout(JSON.parse(await fs.readFile(this.filePath, "utf8")));
        } catch (error) {
            if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
            return readLayout(null);
        }
    }

    save(layout: GraphLayout, changes: Record<string, PresentationState | null> = {}, order?: string[]): Promise<GraphViewState> {
        const { presentationStates: _states, laneOrder: _order, ...graph } = readLayout(layout);
        const edits = Object.entries(changes);
        const savedEdits = readPresentationStates(changes);
        const savedOrder = order === undefined ? undefined : readGraphViewState({ laneOrder: order }).laneOrder;
        const next = this.pending.catch(() => {}).then(async () => {
            const current = await this.load();
            const merged = new Map(Object.entries(current.presentationStates));
            for (const [key, view] of edits) {
                if (view === null) merged.delete(key);
                else if (Object.hasOwn(savedEdits, key)) merged.set(key, savedEdits[key]);
            }
            const result = { presentationStates: Object.fromEntries(merged), laneOrder: savedOrder ?? current.laneOrder };
            if (isDeepStrictEqual({ ...graph, ...result }, current)) return result;
            const text = JSON.stringify({ ...graph, ...(merged.size ? { presentationStates: result.presentationStates } : {}),
                ...(result.laneOrder.length ? { laneOrder: result.laneOrder } : {}) }, null, 2) + "\n";
            await fs.mkdir(path.dirname(this.filePath), { recursive: true });
            const temporary = `${this.filePath}.${randomUUID()}.tmp`;
            try {
                await fs.writeFile(temporary, text, "utf8");
                await fs.rename(temporary, this.filePath);
            } finally {
                await fs.rm(temporary, { force: true });
            }
            return result;
        });
        this.pending = next;
        return next;
    }
}
