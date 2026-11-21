import { spawn } from "node:child_process";
import { dirname } from "node:path";

export function spawnWorkspaceServer(binary: string, project: string, log: number,
    env: NodeJS.ProcessEnv = process.env) {
    return spawn(binary, ["--server", "--workspace-root", project, "--rpc-fd", "3"], {
        cwd: project, env: { ...env, INTRAVENOUS_DIR: dirname(binary) },
        stdio: ["ignore", log, log, "pipe"],
    });
}
