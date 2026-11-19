import * as fs from "node:fs/promises";
import * as path from "node:path";
import { randomUUID } from "node:crypto";
import { isDeepStrictEqual } from "node:util";
import { readPresentationStates, readGraphViewState, readLayout, type PresentationState,
    type GraphLayout, type GraphViewState } from "./graphScope";
import { readTimelineSnapMemory, type TimelineSnapMemory } from "./timelineSnap";

// Frontend-owned data, separate from iv_project.jsonl. Serialize saves and
// replace the complete file so a reload cannot read half a layout.
export class GraphLayoutStore {
    private pending: Promise<unknown> = Promise.resolve();
    readonly filePath: string;

    constructor(projectDirectory: string) {
        this.filePath = path.join(projectDirectory, ".intravenous", "graph-layout.json");
    }

    async load(): Promise<GraphLayout> {
        return (await this.read()).layout;
    }

    private async read(): Promise<{ layout: GraphLayout; timelineSnap: TimelineSnapMemory }> {
        try {
            const value = JSON.parse(await fs.readFile(this.filePath, "utf8"));
            return { layout: readLayout(value), timelineSnap: readTimelineSnapMemory(value?.timelineSnap) };
        } catch (error) {
            if ((error as NodeJS.ErrnoException).code !== "ENOENT") throw error;
            return { layout: readLayout(null), timelineSnap: {} };
        }
    }

    private enqueue<T>(action: () => Promise<T>): Promise<T> {
        const next = this.pending.catch(() => {}).then(action);
        this.pending = next;
        return next;
    }

    private async write(layout: GraphLayout, timelineSnap: TimelineSnapMemory): Promise<void> {
        const { presentationStates, laneOrder, ...geometry } = layout;
        const text = JSON.stringify({ ...geometry, ...(Object.keys(presentationStates).length ? { presentationStates } : {}),
            ...(laneOrder.length ? { laneOrder } : {}), ...(Object.keys(timelineSnap).length ? { timelineSnap } : {}) }, null, 2) + "\n";
        await fs.mkdir(path.dirname(this.filePath), { recursive: true });
        const temporary = `${this.filePath}.${randomUUID()}.tmp`;
        try {
            await fs.writeFile(temporary, text, "utf8");
            await fs.rename(temporary, this.filePath);
        } finally {
            await fs.rm(temporary, { force: true });
        }
    }

    prepareTimelineSnap<T extends { memory: TimelineSnapMemory }>(prepare: (memory: TimelineSnapMemory) => T): Promise<T> {
        return this.enqueue(async () => {
            const { layout, timelineSnap } = await this.read();
            const next = prepare(timelineSnap);
            if (!isDeepStrictEqual(next.memory, timelineSnap)) await this.write(layout, next.memory);
            return next;
        });
    }

    save(layout: GraphLayout, changes: Record<string, PresentationState | null> = {}, order?: string[]): Promise<GraphViewState> {
        const { presentationStates: _states, laneOrder: _order, ...graph } = readLayout(layout);
        const edits = Object.entries(changes);
        const savedEdits = readPresentationStates(changes);
        const savedOrder = order === undefined ? undefined : readGraphViewState({ laneOrder: order }).laneOrder;
        return this.enqueue(async () => {
            const { layout: current, timelineSnap } = await this.read();
            const merged = new Map(Object.entries(current.presentationStates));
            for (const [key, view] of edits) {
                if (view === null) merged.delete(key);
                else if (Object.hasOwn(savedEdits, key)) merged.set(key, savedEdits[key]);
            }
            const result = { presentationStates: Object.fromEntries(merged), laneOrder: savedOrder ?? current.laneOrder };
            const next = { ...graph, ...result };
            if (!isDeepStrictEqual(next, current)) await this.write(next, timelineSnap);
            return result;
        });
    }
}
