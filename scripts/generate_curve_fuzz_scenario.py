#!/usr/bin/env python3
"""Generate a reproducible Automation Curve scenario for headlessClient present."""

import argparse
import json
import random
from pathlib import Path


def signed_sample(index):
    return str((index + (1 << 63)) % (1 << 64) - (1 << 63))


def scenario(seed):
    rng = random.Random(seed)
    fixture = Path(__file__).resolve().parents[1] / "examples/wasm-presentation/placement-scenario.json"
    result = json.loads(fixture.read_text())
    result["presentationId"] = "builtin/automation_curve"
    result["seed"] = seed
    inputs = []

    def add(at_ms, **value):
        inputs.append({"atMs": at_ms, "value": value})

    def gesture(at_ms, phase, x, y):
        add(at_ms, type="gesture", id="2", phase=phase, point={"x": x, "y": y},
            modifiers={"shift": rng.choice((False, True)), "alt": False, "control": False, "meta": False})

    def viewport(index, span, width, height):
        return {"width": width, "height": height, "time": {
            "beginSample": signed_sample(index), "beginFraction": 0, "spanSamples": span, "sampleRate": 48000,
        }}

    result["snapshot"]["viewport"] = viewport((1 << 63) - 2048, 4096, 500, 140)
    for turn in range(16):
        at_ms = 200 + turn * 330
        anchor = rng.choice((0, 1 << 63, 1 << 64))
        span = rng.choice((64, 4096, 480000, 1 << 32))
        start = anchor - rng.randrange(1, span)
        width, height = rng.choice((320, 500, 900)), rng.choice((100, 140, 230))
        add(at_ms, type="viewport", viewport=viewport(start, span, width, height))
        add(at_ms + 17, type="valueView", action="zoom", y=rng.random(), factor=rng.choice((0.7, 1.4)))
        x, y = width * rng.uniform(.2, .8), height * rng.uniform(.2, .8)
        gesture(at_ms + 40, "begin", x, y)
        for move in range(4):
            x += rng.randint(-width // 8, width // 8)
            y += rng.randint(-height // 4, height // 4)
            gesture(at_ms + 55 + move * 9, "move", x, y)
        add(at_ms + 94, type="viewport", viewport=viewport(start + span // 3, span, width, height))
        outcome = rng.randrange(5)
        if outcome == 0:
            add(at_ms + 125, type="interrupt", reason="history")
        else:
            gesture(at_ms + 125, "cancel" if outcome == 1 else "finish", x, y)
        add(at_ms + 240, type="valueView", action="pan", fraction=rng.uniform(-.25, .25))
    result["inputs"] = inputs
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("seed", type=int, help="Use the same seed to regenerate the scenario")
    args = parser.parse_args()
    print(json.dumps(scenario(args.seed), indent=2))
