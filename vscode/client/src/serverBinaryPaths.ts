import * as path from "path";
import * as fs from "fs";

declare const __INTRAVENOUS_DEFAULT_DIR__: string;

const packagedDefaultServerDir = typeof __INTRAVENOUS_DEFAULT_DIR__ === "string"
    ? __INTRAVENOUS_DEFAULT_DIR__ : "";

export function resolveIntravenousBinary(name: string, workspaceRoot?: string, configuredDir?: string): { source: string; path: string } {
    const envDir = process.env.INTRAVENOUS_DIR;
    if (envDir) {
        if (!path.isAbsolute(envDir)) throw new Error(`INTRAVENOUS_DIR must be absolute: ${envDir}`);
        return { source: "INTRAVENOUS_DIR", path: path.join(envDir, name) };
    }
    // An installation's packaged default takes precedence over stale workspace configuration.
    if (packagedDefaultServerDir) return { source: "client build default", path: path.join(packagedDefaultServerDir, name) };
    if (configuredDir) {
        if (!path.isAbsolute(configuredDir)) throw new Error(`intravenous.intravenousDir must be absolute: ${configuredDir}`);
        return { source: "intravenous.intravenousDir", path: path.join(configuredDir, name) };
    }
    const candidates = workspaceRoot ? autoDetectedServerDirectoriesForWorkspaceRoot(workspaceRoot) : [];
    for (const candidate of candidates) {
        const file = path.join(candidate.directory, name);
        try {
            if (fs.statSync(file).isFile()) return { source: candidate.source, path: file };
        } catch {}
    }
    throw new Error(
        "Intravenous executable directory is not configured. " +
        "Set INTRAVENOUS_DIR or intravenous.intravenousDir, " +
        "or build the repo so the client can auto-detect build/src/intravenous.",
    );
}

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
