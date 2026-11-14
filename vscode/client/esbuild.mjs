import esbuild from "esbuild";
import path from "path";
import { fileURLToPath } from "url";
import { build as buildWebview } from "vite";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const watch = process.argv.includes("--watch");

const outdir = path.join(__dirname, "dist");
const webviewConfigFile = path.join(__dirname, "vite.webview.config.mts");

// The extension host is a Node bundle. The webview is a separate Vue
// application built by Vite into dist/webview (see vite.webview.config.mts).
const context = await esbuild.context({
    entryPoints: [path.join(__dirname, "src", "extension.ts")],
    outfile: path.join(outdir, "extension.js"),
    bundle: true,
    platform: "node",
    format: "cjs",
    target: "node22",
    sourcemap: true,
    tsconfig: path.join(__dirname, "tsconfig.json"),
    external: ["vscode"],
    define: {
        "process.env.NODE_ENV": JSON.stringify(process.env.NODE_ENV || "development"),
        "__INTRAVENOUS_DEFAULT_DIR__": JSON.stringify(process.env.INTRAVENOUS_DEFAULT_DIR || ""),
    },
});

if (watch) {
    await context.watch();
    await buildWebview({ configFile: webviewConfigFile, build: { watch: {} } });
    console.log("Intravenous VS Code client watch started.");
} else {
    await context.rebuild();
    await context.dispose();
    await buildWebview({ configFile: webviewConfigFile });
    console.log("Intravenous VS Code client build complete.");
}
