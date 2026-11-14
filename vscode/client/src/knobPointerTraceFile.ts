import * as fs from "fs";
import * as path from "path";

export function knobPointerTraceDirectory(projectRoot: string): string {
    const projectDirectory = path.resolve(projectRoot);
    let directory = projectDirectory;
    for (;;) {
        if (fs.existsSync(path.join(directory, ".git"))) return path.join(directory, "debug-release");
        const parent = path.dirname(directory);
        if (parent === directory) return path.join(projectDirectory, "debug-release");
        directory = parent;
    }
}
