import path from "node:path";
import { fileURLToPath } from "node:url";

import vue from "@vitejs/plugin-vue";
import { defineConfig } from "vite";

const clientRoot = path.dirname(fileURLToPath(import.meta.url));

// Builds the Vue webview application into dist/webview. The extension host
// writes the webview HTML itself (CSP nonce and webview resource URIs), so the
// bundle uses one JavaScript entry with fixed output names instead of an HTML
// entry.
export default defineConfig({
    root: path.join(clientRoot, "webview"),
    base: "./",
    plugins: [vue()],
    publicDir: false,
    build: {
        outDir: path.join(clientRoot, "dist", "webview"),
        emptyOutDir: true,
        target: "es2022",
        cssCodeSplit: false,
        modulePreload: false,
        sourcemap: true,
        rollupOptions: {
            input: path.join(clientRoot, "webview", "main.ts"),
            output: {
                entryFileNames: "main.js",
                chunkFileNames: "[name].js",
                assetFileNames: (asset) => {
                    const name = asset.names?.[0] ?? asset.name ?? "";
                    return name.endsWith(".css") ? "main.css" : "assets/[name][extname]";
                },
            },
        },
    },
});
