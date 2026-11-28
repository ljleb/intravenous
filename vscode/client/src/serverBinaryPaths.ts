import * as path from "path";

export function resolveIntravenousBinary(name: string, configuredDir?: string,
    bundledRuntime?: string): { source: string; path: string } {
    if (bundledRuntime) return { source: "development bundle", path: path.join(bundledRuntime, name) };
    const envDir = process.env.INTRAVENOUS_DIR;
    if (envDir) {
        if (!path.isAbsolute(envDir)) throw new Error(`INTRAVENOUS_DIR must be absolute: ${envDir}`);
        return { source: "INTRAVENOUS_DIR", path: path.join(envDir, name) };
    }
    if (configuredDir) {
        if (!path.isAbsolute(configuredDir)) throw new Error(`intravenous.intravenousDir must be absolute: ${configuredDir}`);
        return { source: "intravenous.intravenousDir", path: path.join(configuredDir, name) };
    }
    throw new Error(
        "Intravenous executable directory is not configured. "
        + "Launch with nix develop --command ./scripts/run_dev.sh PROJECT_DIRECTORY, "
        + "or set INTRAVENOUS_DIR or intravenous.intravenousDir.",
    );
}
