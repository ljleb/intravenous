import test from "node:test";
import assert from "node:assert/strict";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";

import {
    canonicalProjectRoot,
    projectSessionDescriptorPath,
    readProjectSessionDescriptor,
} from "../src/projectSessionDiscovery";

test("project session discovery accepts only the matching canonical project", () => {
    const temporary = fs.mkdtempSync(path.join(os.tmpdir(), "iv-session-discovery-"));
    const workspace = path.join(temporary, "workspace");
    const runtime = path.join(temporary, "runtime");
    fs.mkdirSync(workspace);
    fs.mkdirSync(runtime);
    const previousRuntime = process.env.XDG_RUNTIME_DIR;
    process.env.XDG_RUNTIME_DIR = runtime;
    try {
        const descriptorPath = projectSessionDescriptorPath(workspace);
        fs.mkdirSync(path.dirname(descriptorPath), { recursive: true });
        const descriptor = {
            protocolVersion: 1,
            pid: 42,
            startupTimeUnixMs: 100,
            projectRoot: canonicalProjectRoot(workspace),
            endpoint: {
                type: "unix",
                path: path.join(path.dirname(descriptorPath), "server.sock"),
            },
            localCapability: "local-secret",
        };
        fs.writeFileSync(descriptorPath, JSON.stringify(descriptor));

        assert.deepEqual(readProjectSessionDescriptor(workspace), descriptor);

        fs.writeFileSync(descriptorPath, JSON.stringify({
            ...descriptor,
            projectRoot: path.join(temporary, "another-project"),
        }));
        assert.equal(readProjectSessionDescriptor(workspace), null);
    } finally {
        if (previousRuntime === undefined) delete process.env.XDG_RUNTIME_DIR;
        else process.env.XDG_RUNTIME_DIR = previousRuntime;
        fs.rmSync(temporary, { recursive: true, force: true });
    }
});
