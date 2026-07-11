import esbuild from "esbuild";
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const watch = process.argv.includes("--watch");

const outdir = path.join(__dirname, "dist");
const mediaSource = path.join(__dirname, "media");
const mediaDest = path.join(outdir, "media");

function copyMedia() {
    fs.mkdirSync(outdir, { recursive: true });
    if (fs.existsSync(mediaSource)) {
        fs.cpSync(mediaSource, mediaDest, { recursive: true });
    }
}

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
    loader: {
        ".svg": "file",
    },
    define: {
        "process.env.NODE_ENV": JSON.stringify(process.env.NODE_ENV || "development"),
    },
});

copyMedia();

if (watch) {
    await context.watch();
    console.log("Intravenous VS Code client esbuild watch started.");
} else {
    await context.rebuild();
    await context.dispose();
    console.log("Intravenous VS Code client build complete.");
}
