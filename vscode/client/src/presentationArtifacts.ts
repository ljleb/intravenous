import { createHash } from "node:crypto";
import { readPresentationManifest, type PresentationArtifact } from "./presentationPlugin";
import { readWasmPresentationDescriptor } from "./wasmPresentation";
import { recordedTriggerBinsOperation } from "./observations";

export interface PresentationArtifactRpc {
    listPresentations(): Promise<{ ready: boolean; entries: unknown[] }>;
    fetchPresentation(digest: string): Promise<{ digest: string; bytes: string }>;
}

const maximumArtifactBytes = 256 * 1024;

export class PresentationArtifactReader {
    private blobs = new Map<string, Promise<{ bytes: string; descriptor: ReturnType<typeof readWasmPresentationDescriptor> }>>();
    private published = new Map<string, { binding: string; artifact: PresentationArtifact }>();
    private readId = 0;

    async read(rpc: PresentationArtifactRpc, report: (error: string) => void): Promise<{
        ready: boolean; artifacts: PresentationArtifact[];
    }> {
        const idAtStart = ++this.readId;
        const { ready, entries } = await rpc.listPresentations();
        if (typeof ready !== "boolean" || !Array.isArray(entries) || entries.length > 32)
            throw new Error("Invalid presentation catalog");
        const pending = new Map(this.blobs);
        const seen = new Set<string>();
        const artifacts: PresentationArtifact[] = [];
        const published = new Map<string, { binding: string; artifact: PresentationArtifact }>();
        for (const entry of entries) {
            if (!entry || typeof entry !== "object") continue;
            const { id, provider, digest, format, schemaVersion, manifest, error } = entry as Record<string, unknown>;
            if (typeof error === "string") report(error);
            const parsed = readPresentationManifest(manifest);
            if (!parsed || typeof provider !== "string" || !/^[A-Za-z0-9_.-]+$/.test(provider)
                || id !== `${provider}/${parsed.id}` || format !== "wasm" || schemaVersion !== parsed.version
                || typeof digest !== "string" || !/^[0-9a-f]{64}$/.test(digest)) continue;
            const identity = id as string;
            if (parsed.observations?.some(({ operation }) => operation !== recordedTriggerBinsOperation) || parsed.timelineSnapping) {
                report(`${identity} · This presentation ABI supports only recorded trigger observations`);
                continue;
            }
            if (seen.has(identity)) continue;
            seen.add(identity);
            const binding = JSON.stringify(parsed);
            let bytes = pending.get(digest);
            if (!bytes) {
                bytes = this.blobs.get(digest) ?? this.fetch(rpc, digest);
                pending.set(digest, bytes);
            }
            try {
                const artifact = await bytes;
                const program = { id: identity, manifest: { ...parsed, ...artifact.descriptor }, digest, bytes: artifact.bytes };
                published.set(identity, { binding, artifact: program });
                artifacts.push(program);
            }
            catch (error) {
                pending.delete(digest);
                report(`${identity} · ${String(error)}`);
                const previous = this.published.get(identity);
                if (previous?.binding === binding) {
                    published.set(identity, previous);
                    artifacts.push(previous.artifact);
                }
            }
        }
        if (idAtStart === this.readId) {
            const digests = new Set(artifacts.map(({ digest }) => digest));
            this.blobs = new Map([...pending].filter(([digest]) => digests.has(digest)));
            this.published = published;
        }
        return { ready, artifacts };
    }

    private async fetch(rpc: PresentationArtifactRpc, digest: string) {
        const result = await rpc.fetchPresentation(digest);
        const encoded = result.bytes;
        if (result.digest !== digest || typeof encoded !== "string" || encoded.length > 4 * Math.ceil(maximumArtifactBytes / 3))
            throw new Error("Invalid presentation artifact");
        const bytes = Buffer.from(encoded, "base64");
        if (bytes.length > maximumArtifactBytes || bytes.toString("base64") !== encoded
            || createHash("sha256").update(bytes).digest("hex") !== digest)
            throw new Error("Presentation artifact digest mismatch");
        return { bytes: encoded, descriptor: readWasmPresentationDescriptor(bytes) };
    }
}
