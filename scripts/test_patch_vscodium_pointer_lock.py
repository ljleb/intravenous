"""Run with python3 scripts/test_patch_vscodium_pointer_lock.py (requires Node.js)."""

import json
import subprocess
import unittest

from patch_vscodium_pointer_lock import patch_bundle


# The installed bundle's two policies, with their original minified names. Keep
# the registry initialization so the patch also verifies that prerequisite.
BUNDLE = '''
class CodeApplication {
configureSession(){const e=f=>f?.startsWith(`${T.vscodeFileResource}://${Oa}`),i=f=>f?.startsWith(`${T.vscodeWebview}://`),s=new Set(["pointerLock","notifications"]),r=new Set([...s,"clipboard-read","clipboard-sanitized-write","deprecated-sync-clipboard-read"]),n=new Set([...s,"media","local-fonts","deprecated-sync-clipboard-read"]);fa.defaultSession.setPermissionRequestHandler((f,p,m,w)=>i(w.requestingUrl)?m(r.has(p)):e(w.requestingUrl)?m(n.has(p)):m(!1)),fa.defaultSession.setPermissionCheckHandler((f,p,m,w)=>i(w.requestingUrl)?r.has(p):e(w.requestingUrl)?n.has(p):!1);}
openFirstWindow(e){this.auxiliaryWindowsMainService=e.get(Kr);}
}
'''


def decisions(bundle: str) -> list:
    probe = '''
const T = {vscodeFileResource: "vscode-file", vscodeWebview: "vscode-webview"};
const Oa = "vscode-app";
let request, check;
const fa = {defaultSession: {
    setPermissionRequestHandler(handler) { request = handler; },
    setPermissionCheckHandler(handler) { check = handler; },
}};
'''
    probe += bundle
    probe += '''
const app = new CodeApplication();
app.auxiliaryWindowsMainService = { getWindowByWebContents(wc) { return wc.registered; } };
app.configureSession();
const results = [];
for (const [url, mainFrame, wc, permission] of [
    ["about:blank", true, {registered: true}, "pointerLock"],
    ["about:blank", true, {registered: false}, "pointerLock"],
    ["about:blank", true, null, "pointerLock"],
    ["about:blank", false, {registered: true}, "pointerLock"],
    ["about:blank", true, {registered: true}, "media"],
    ["about:blank", true, {registered: true}, "notifications"],
    ["https://example.com/", true, {registered: true}, "pointerLock"],
    ["vscode-webview://test/index.html", false, {}, "pointerLock"],
    ["vscode-file://vscode-app/workbench.html", true, {}, "media"],
    ["vscode-webview://test/index.html", false, {}, "media"],
]) {
    const details = {requestingUrl: url, isMainFrame: mainFrame};
    let granted;
    request(wc, permission, value => granted = value, details);
    results.push([granted, check(wc, permission, "unused", details)]);
}
app.auxiliaryWindowsMainService = undefined;
results.push([check({registered: true}, "pointerLock", "unused", {
    requestingUrl: "about:blank", isMainFrame: true,
})]);
console.log(JSON.stringify(results));
'''
    result = subprocess.run(["node", "--input-type=commonjs", "-"], input=probe,
                            text=True, capture_output=True, check=True)
    return json.loads(result.stdout)


class DetachedPointerLockPatchTests(unittest.TestCase):
    def test_only_registered_main_frame_pointer_lock_gains_permission(self):
        before = decisions(BUNDLE)
        after = decisions(patch_bundle(BUNDLE))
        self.assertEqual(before[0], [False, False])
        self.assertEqual(after[0], [True, True])
        self.assertEqual(after[1:], before[1:])
        self.assertEqual(after[1:7], [[False, False]] * 6)
        self.assertEqual(after[7:10], [[True, True], [True, True], [False, False]])
        self.assertEqual(after[10], [False])

    def test_missing_registry_or_unknown_handler_is_rejected(self):
        for source in (BUNDLE.replace("this.auxiliaryWindowsMainService=e.get(Kr);", ""),
                       BUNDLE.replace("m(!1)", "m(true)"), BUNDLE + BUNDLE):
            with self.assertRaises(ValueError):
                patch_bundle(source)

    def test_request_and_check_must_share_the_same_policy(self):
        with self.assertRaises(ValueError):
            patch_bundle(BUNDLE.replace("?r.has(p):", "?different.has(p):"))

    def test_a_second_patch_is_rejected(self):
        with self.assertRaises(ValueError):
            patch_bundle(patch_bundle(BUNDLE))


if __name__ == "__main__":
    unittest.main()
