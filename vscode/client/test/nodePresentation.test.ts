import test from "node:test";
import assert from "node:assert/strict";

import {
    frameworkDefaultNodePresentationProvider,
    NodePresentationDescriptor,
    nodePresentationDisplayModes,
    NodePresentationRegistry,
} from "../src/nodePresentation";

type TestProvider = {
    presentationId: string;
    renderer: string;
};

const descriptors: NodePresentationDescriptor[] = [{
    definitionId: "iv.test.filter",
    presentationId: "iv.test.knobs",
    displayName: "Knobs",
}, {
    definitionId: "iv.test.filter",
    presentationId: "iv.test.plot",
    displayName: "Response plot",
}, {
    definitionId: "iv.test.oscillator",
    presentationId: "iv.test.knobs",
    displayName: "Oscillator knobs",
}];

function registry(): NodePresentationRegistry<TestProvider> {
    return new NodePresentationRegistry<TestProvider>({
        presentationId: frameworkDefaultNodePresentationProvider.presentationId,
        renderer: "default",
    });
}

test("node presentation display modes are one four-way host choice", () => {
    assert.deepEqual(nodePresentationDisplayModes, [
        "compact",
        "nano",
        "mini",
        "editable",
    ]);
});

test("NodePresentationRegistry intersects backend descriptors with frontend providers", () => {
    const presentations = registry();
    presentations.register({
        presentationId: "iv.test.knobs",
        renderer: "knobs",
    });

    assert.deepEqual(
        presentations.available("iv.test.filter", descriptors),
        [descriptors[0]],
    );
    assert.equal(
        presentations.select("iv.test.filter", descriptors).provider.renderer,
        "knobs",
    );
    assert.equal(
        presentations.select("iv.test.oscillator", descriptors).provider.renderer,
        "knobs",
    );
});

test("NodePresentationRegistry honors an available preferred presentation", () => {
    const presentations = registry();
    presentations.register({
        presentationId: "iv.test.knobs",
        renderer: "knobs",
    });
    presentations.register({
        presentationId: "iv.test.plot",
        renderer: "plot",
    });

    const selected = presentations.select(
        "iv.test.filter",
        descriptors,
        "iv.test.plot",
    );
    assert.equal(selected.descriptor?.presentationId, "iv.test.plot");
    assert.equal(selected.provider.renderer, "plot");
    assert.equal(selected.usedDefault, false);

    const unavailablePreference = presentations.select(
        "iv.test.filter",
        descriptors,
        "iv.test.unavailable",
    );
    assert.equal(unavailablePreference.descriptor?.presentationId, "iv.test.knobs");
});

test("NodePresentationRegistry uses the guaranteed frontend default fallback", () => {
    const selected = registry().select("iv.test.filter", descriptors);

    assert.equal(selected.descriptor, null);
    assert.equal(selected.provider.renderer, "default");
    assert.equal(selected.usedDefault, true);
});

test("NodePresentationRegistry rejects duplicate frontend provider IDs", () => {
    const presentations = registry();
    presentations.register({
        presentationId: "iv.test.knobs",
        renderer: "first",
    });

    assert.throws(() => presentations.register({
        presentationId: "iv.test.knobs",
        renderer: "second",
    }), /already registered/);
});
