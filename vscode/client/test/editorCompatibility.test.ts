import test from "node:test";
import assert from "node:assert/strict";
import { knobPointerLockVersionWarning } from "../src/editorCompatibility";

test("older editor warnings identify the core version and the main-window fallback", () => {
    for (const version of ["1.85.0", "1.116.0", "1.126.0", "1.132.99"]) {
        const warning = knobPointerLockVersionWarning("VSCodium", version);
        assert.ok(warning);
        assert.ok(warning.includes(`VSCodium uses VS Code ${version}`));
        assert.match(warning, /1\.133\.0 or newer/);
        assert.match(warning, /keep the graph in the main editor window/);
    }
});

test("the fixed release and newer editor versions do not warn", () => {
    for (const version of ["1.133.0", "1.133.0+build.1", "1.133.1", "1.143.1", "1.143.17232", "1.134.0-insider", "2.0.0"]) {
        assert.equal(knobPointerLockVersionWarning("Visual Studio Code", version), null);
    }
});

test("a prerelease of the first fixed version is not assumed to contain the release fix", () => {
    assert.ok(knobPointerLockVersionWarning("Code - Insiders", "1.133.0-insider"));
});

test("unrecognized custom version labels do not produce an unsupported-version claim", () => {
    for (const version of ["", "development", "1.133", "release-1.143.1"]) {
        assert.equal(knobPointerLockVersionWarning("Custom Editor", version), null);
    }
});
