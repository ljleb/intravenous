#!/usr/bin/env python3

import csv
import decimal
import pathlib
import statistics
import sys


def median(rows: list[dict[str, str]], field: str) -> decimal.Decimal:
    return statistics.median(decimal.Decimal(row[field]) for row in rows)


def number(value: decimal.Decimal) -> str:
    return format(value.normalize(), "f")


def write_summary(
    path: pathlib.Path,
    groups: dict[tuple[str, ...], list[dict[str, str]]],
    key_names: list[str],
    fields: list[str],
    text_fields: list[str],
) -> None:
    with path.open("w", newline="") as output:
        writer = csv.writer(output, delimiter="\t", lineterminator="\n")
        writer.writerow([*key_names, "measurements", *fields, *text_fields])
        for key in sorted(groups):
            rows = groups[key]
            writer.writerow(
                [
                    *key,
                    len(rows),
                    *(number(median(rows, field)) for field in fields),
                    *(
                        ",".join(sorted({row[field] for row in rows}))
                        for field in text_fields
                    ),
                ]
            )


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: summarize_realtime_storage_cost_sweep.py RESULTS.tsv OUTPUT_DIR",
            file=sys.stderr,
        )
        return 1

    results_path = pathlib.Path(sys.argv[1])
    output_dir = pathlib.Path(sys.argv[2])
    output_dir.mkdir(parents=True, exist_ok=True)
    with results_path.open(newline="") as source:
        rows = list(csv.DictReader(source, delimiter="\t"))

    # Tick and edit fields are repeated when one edit caused several GraphJit
    # compilations. Keep one copy per logical edit before taking medians.
    edits: dict[tuple[str, str, str, str], dict[str, str]] = {}
    for row in rows:
        if row["published"] != "1" or row["accepted"] != "1":
            continue
        key = (row["profile"], row["case"], row["iteration"], row["edit"])
        edits.setdefault(key, row)

    tick_groups: dict[tuple[str, ...], list[dict[str, str]]] = {}
    for row in edits.values():
        key = (row["profile"], row["case"], row["edit"])
        tick_groups.setdefault(key, []).append(row)
    write_summary(
        output_dir / "tick-edit-summary.tsv",
        tick_groups,
        ["profile", "case", "edit"],
        [
            "wall_ms",
            "configure_ms",
            "graph_jit_ms",
            "stage_ms",
            "tick_mean_us",
            "tick_p50_us",
            "tick_p95_us",
            "tick_p99_us",
            "tick_max_us",
            "block_period_us",
        ],
        [],
    )

    plan_groups: dict[tuple[str, ...], list[dict[str, str]]] = {}
    for row in rows:
        if row["accepted"] != "1" or row["compile_present"] != "1":
            continue
        key = (
            row["profile"],
            row["case"],
            row["edit"],
            row["compile_transition"],
        )
        plan_groups.setdefault(key, []).append(row)
    write_summary(
        output_dir / "plan-summary.tsv",
        plan_groups,
        ["profile", "case", "edit", "transition"],
        [
            "compile_total_us",
            "selected_decisions",
            "transient_stack_decisions",
            "stack_with_persistent_carry_decisions",
            "full_node_storage_decisions",
            "selected_copied_bytes",
            "selected_ring_addressed_bytes",
            "selected_stack_bytes",
            "selected_persistent_bytes",
            "selected_weighted_cost",
            "packed_root_stack_bytes",
            "sample_persistent_allocation_bytes",
            "event_persistent_allocation_bytes",
        ],
        ["selection_signature"],
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
