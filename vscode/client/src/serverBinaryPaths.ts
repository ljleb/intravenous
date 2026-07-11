import * as path from "path";

export type ServerBinaryDirectoryCandidate = {
    source: string;
    directory: string;
};

export function autoDetectedServerDirectoriesForWorkspaceRoot(
    workspaceRoot: string,
): ServerBinaryDirectoryCandidate[] {
    let current = workspaceRoot;
    let depth = 0;
    let directories: ServerBinaryDirectoryCandidate[] = [];

    while (true) {
        directories.push({
            source: depth === 0
                ? "auto-detected build/src/intravenous"
                : `auto-detected parent build/src/intravenous (${current})`,
            directory: path.join(current, "build", "src", "intravenous"),
        });
        directories.push({
            source: depth === 0
                ? "auto-detected build/intravenous"
                : `auto-detected parent build/intravenous (${current})`,
            directory: path.join(current, "build", "intravenous"),
        });

        const parent = path.dirname(current);
        if (parent === current) {
            break;
        }

        current = parent;
        depth += 1;
    }

    return directories;
}
