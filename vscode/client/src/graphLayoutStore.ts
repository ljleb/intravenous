import * as fs from "node:fs/promises";
import * as path from "node:path";
import { randomUUID } from "node:crypto";
import { readLayout, type GraphLayout } from "./graphScope";

// Frontend-owned data, separate from iv_project.jsonl. Serialize saves and
// replace the complete file so a reload cannot read half a layout.
export class GraphLayoutStore {
    private pending: Promise<void> = Promise.resolve();
    readonly filePath: string;

    constructor(projectDirectory: string) {
        this.filePath = path.join(projectDirectory, ".intravenous", "graph-layout.json");
    }

    async load(): Promise<GraphLayout> {
        try {
            return readLayout(JSON.parse(await fs.readFile(this.filePath, "utf8")));
        } catch (error) {
            if ((error as NodeJS.ErrnoException).code === "ENOENT") return readLayout(null);
            throw error;
        }
    }

    save(layout: GraphLayout): Promise<void> {
        const text = JSON.stringify(readLayout(layout), null, 2) + "\n";
        const next = this.pending.catch(() => {}).then(async () => {
            await fs.mkdir(path.dirname(this.filePath), { recursive: true });
            const temporary = `${this.filePath}.${randomUUID()}.tmp`;
            try {
                await fs.writeFile(temporary, text, "utf8");
                await fs.rename(temporary, this.filePath);
            } finally {
                await fs.rm(temporary, { force: true });
            }
        });
        this.pending = next;
        return next;
    }
}
