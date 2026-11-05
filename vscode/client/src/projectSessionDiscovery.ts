import * as fs from "fs";
import * as path from "path";

export type ProjectSessionDescriptor = {
    protocolVersion: number;
    pid: number;
    startupTimeUnixMs: number;
    projectRoot: string;
    endpoint: {
        type: "unix";
        path: string;
    };
    localCapability: string;
};

export function canonicalProjectRoot(projectRoot: string): string {
    return path.normalize(fs.realpathSync.native(projectRoot));
}

export function projectSessionHash(projectRoot: string): string {
    let hash = 14695981039346656037n;
    const prime = 1099511628211n;
    for (const byte of Buffer.from(canonicalProjectRoot(projectRoot), "utf8")) {
        hash ^= BigInt(byte);
        hash = BigInt.asUintN(64, hash * prime);
    }
    return hash.toString(16).padStart(16, "0");
}

export function projectSessionDescriptorPath(projectRoot: string): string {
    const runtimeRoot = process.env.XDG_RUNTIME_DIR
        ? path.join(process.env.XDG_RUNTIME_DIR, "intravenous")
        : path.join("/tmp", `intravenous-${process.getuid?.() ?? 0}`);
    return path.join(runtimeRoot, projectSessionHash(projectRoot), "descriptor.json");
}

export function readProjectSessionDescriptor(
    projectRoot: string,
): ProjectSessionDescriptor | null {
    const descriptorPath = projectSessionDescriptorPath(projectRoot);
    let value: unknown;
    try {
        value = JSON.parse(fs.readFileSync(descriptorPath, "utf8"));
    } catch {
        return null;
    }
    if (!value || typeof value !== "object") return null;
    const descriptor = value as Partial<ProjectSessionDescriptor>;
    const endpoint = descriptor.endpoint;
    if (descriptor.protocolVersion !== 1
        || descriptor.projectRoot !== canonicalProjectRoot(projectRoot)
        || typeof descriptor.pid !== "number"
        || typeof descriptor.startupTimeUnixMs !== "number"
        || typeof descriptor.localCapability !== "string"
        || descriptor.localCapability.length === 0
        || !endpoint
        || endpoint.type !== "unix"
        || typeof endpoint.path !== "string"
        || endpoint.path.length === 0) {
        return null;
    }
    return descriptor as ProjectSessionDescriptor;
}
