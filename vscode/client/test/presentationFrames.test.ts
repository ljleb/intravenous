import assert from "node:assert/strict";
import test from "node:test";
import { PresentationFrames } from "../src/presentationFrames";

function clock() {
    let next = 0;
    const pending = new Map<number, (time: number) => void>();
    const errors: unknown[] = [];
    const frames = new PresentationFrames((run) => { pending.set(++next, run); return next; },
        (id) => pending.delete(id), (error) => errors.push(error));
    function run(time: number) {
        const batch = [...pending.values()];
        pending.clear();
        for (const callback of batch) callback(time);
    }
    return { frames, pending, errors, run };
}

test("presentation frames batch measurements before updates and defer new measurements", () => {
    const { frames, pending, run } = clock();
    const calls: string[] = [];
    const update = (time: number) => { calls.push(`update ${time}`); frames.schedule("measure", measure); };
    const measure = (time: number) => { calls.push(`measure ${time}`); frames.schedule("update", update); };
    frames.schedule("update", update);
    frames.schedule("measure", measure);
    frames.schedule("measure", measure);
    assert.equal(pending.size, 1);
    run(16);
    assert.deepEqual(calls, ["measure 16", "update 16"]);
    assert.equal(pending.size, 1);
    frames.cancel(measure);
    assert.equal(pending.size, 0);
});

test("presentation frame tasks can requeue, cancel earlier work, and recover from an error", () => {
    const { frames, pending, errors, run } = clock();
    const calls: number[] = [];
    const canceled = () => { assert.fail("canceled task ran"); };
    const step = (time: number) => {
        calls.push(time);
        if (time < 30) frames.schedule("update", step);
    };
    frames.schedule("update", step);
    frames.schedule("measure", () => {
        frames.cancel(canceled);
        throw new Error("failed measurement");
    });
    frames.schedule("measure", canceled);
    run(15);
    run(30);
    assert.deepEqual(calls, [15, 30]);
    assert.equal(errors.length, 1);
    assert.equal(pending.size, 0);
});
