import { lstat, readFile, readdir } from "node:fs/promises";
import { join } from "node:path";
import { readPresentationManifest, type PresentationScript } from "./presentationPlugin";

const packagesDirectory = "packages";
const presentationsDirectory = "presentations";
const packagePresentations = `${packagesDirectory}/*/${presentationsDirectory}`;
export const projectPresentationWatchPattern = `{${packagesDirectory},${packagesDirectory}/*,${packagePresentations},${packagePresentations}/*}`;
const maximumProjectManifests = 32;

async function readSmallFile(path: string, limit: number): Promise<string> {
    const info = await lstat(path);
    if (!info.isFile() || info.size > limit) throw new Error("Expected a regular file within the size limit");
    const content = await readFile(path);
    if (content.length > limit) throw new Error("Presentation file exceeded the size limit");
    return content.toString("utf8");
}

async function readDirectory(project: string, relative: string, report: (error: string) => void) {
    const directory = join(project, relative);
    try {
        if (!(await lstat(directory)).isDirectory()) {
            report(`Expected a project directory at ${relative}`);
            return [];
        }
        return await readdir(directory, { withFileTypes: true });
    } catch (error) {
        if ((error as NodeJS.ErrnoException).code !== "ENOENT") report(`${relative} · ${String(error)}`);
        return [];
    }
}

export async function readProjectPresentations(project: string, report: (error: string) => void): Promise<PresentationScript[]> {
    const packages = await readDirectory(project, packagesDirectory, report);
    const plugins: PresentationScript[] = [];
    const ids = new Set<string>();
    let remaining = maximumProjectManifests;
    for (const name of packages.filter((entry) => entry.isDirectory()).map((entry) => entry.name).sort()) {
        const directory = join(packagesDirectory, name, presentationsDirectory);
        const entries = await readDirectory(project, directory, report);
        const manifests = entries.map((entry) => entry.name).filter((file) => file.endsWith(".json")).sort();
        for (const file of manifests) {
            if (remaining === 0) {
                report(`Project presentation limit reached (${maximumProjectManifests} manifests)`);
                return plugins;
            }
            --remaining;
            try {
                const manifest = readPresentationManifest(JSON.parse(await readSmallFile(join(project, directory, file), 16384)));
                if (!manifest) throw new Error("Invalid or unsupported presentation manifest");
                if (ids.has(manifest.id)) throw new Error(`Duplicate presentation ID ${manifest.id}`);
                const source = await readSmallFile(join(project, directory, `${file.slice(0, -5)}.js`), 131072);
                ids.add(manifest.id);
                plugins.push({ manifest, source });
            } catch (error) { report(`${join(directory, file)} · ${error instanceof Error ? error.message : String(error)}`); }
        }
    }
    return plugins;
}
