#!/usr/bin/env python3
from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path
import numpy as np

U_BASE = 8
V_BASE = 2
DISPATCH_U = 8
DISPATCH_V = 2
PARAMETER_COUNT = 24


def merge_model(npz_path: Path):
    z = np.load(npz_path)
    field_maps = []
    spatial_keys = set()
    raw_terms = 0

    for parameter in range(PARAMETER_COUNT):
        items = z[f"field{parameter}_items"]
        controls = z[f"field{parameter}_controls"]
        raw_terms += len(controls)
        merged = defaultdict(float)

        for item, control in zip(items, controls):
            lu, iu, lv, iv, scale = item
            key = (int(lu), int(iu), int(lv), int(iv))
            # Exact algebraic merge: c*s*Bu*Bv.  Multiple tree paths that
            # produce the same tensor B-spline are summed into one field term.
            merged[key] += float(control) * float(scale)

        merged = {k: v for k, v in merged.items() if abs(v) > 1e-12}
        field_maps.append(merged)
        spatial_keys.update(merged)

    keys = sorted(spatial_keys)
    key_to_id = {key: i for i, key in enumerate(keys)}

    basis_terms = [[] for _ in keys]
    for parameter, merged in enumerate(field_maps):
        for key, coefficient in merged.items():
            basis_terms[key_to_id[key]].append((parameter, coefficient))

    return z, raw_terms, keys, basis_terms


def active_in_fine_cell(key, fine_u: int, fine_v: int, u_max: int, v_max: int):
    lu, iu, lv, iv = key

    # A midpoint is exact for support membership inside a finest knot cell:
    # every B-spline restriction is polynomial there and its support boundaries
    # lie on knot lines.
    u = (fine_u + 0.5) / (U_BASE * (1 << u_max))
    v = (fine_v + 0.5) / (V_BASE * (1 << v_max))

    ku = U_BASE * (1 << lu)
    ju = int(np.floor(u * ku))
    u_slot = (iu - ju + 1) & (ku - 1)
    if u_slot > 3:
        return False

    mv = V_BASE * (1 << lv)
    jv = min(int(np.floor(v * mv)), mv - 1)
    v_slot = iv - jv
    return 0 <= v_slot <= 2


def build_dispatch(keys):
    u_max = max(k[0] for k in keys)
    v_max = max(k[2] for k in keys)
    fine_u_count = U_BASE * (1 << u_max)
    fine_v_count = V_BASE * (1 << v_max)

    if fine_u_count % DISPATCH_U or fine_v_count % DISPATCH_V:
        raise RuntimeError("dispatch grid must divide finest knot grid")

    fine_per_dispatch_u = fine_u_count // DISPATCH_U
    fine_per_dispatch_v = fine_v_count // DISPATCH_V

    cells = []
    for cv in range(DISPATCH_V):
        for cu in range(DISPATCH_U):
            candidates = set()
            for fv in range(cv * fine_per_dispatch_v, (cv + 1) * fine_per_dispatch_v):
                for fu in range(cu * fine_per_dispatch_u, (cu + 1) * fine_per_dispatch_u):
                    for basis_id, key in enumerate(keys):
                        if active_in_fine_cell(key, fu, fv, u_max, v_max):
                            candidates.add(basis_id)
            cells.append(tuple(sorted(candidates)))

    offsets = []
    refs = []
    for cell in cells:
        offsets.append((len(refs), len(cell)))
        refs.extend(cell)

    return u_max, v_max, offsets, refs


def write_array(f, ctype, name, values, per_line=12, formatter=str):
    f.write(f"inline constexpr std::array<{ctype}, {len(values)}> {name} {{\n")
    for i in range(0, len(values), per_line):
        chunk = values[i:i+per_line]
        f.write("    " + ", ".join(formatter(x) for x in chunk) + ",\n")
    f.write("};\n\n")


def emit_header(path: Path, source_name: str, raw_terms, keys, basis_terms, u_max, v_max, cells, refs):
    basis_u_level = []
    basis_u_index = []
    basis_v_level = []
    basis_v_index = []
    basis_term_offset = []
    basis_term_count = []
    term_parameter = []
    term_coefficient = []

    for key, terms in zip(keys, basis_terms):
        lu, iu, lv, iv = key
        basis_u_level.append(lu)
        basis_u_index.append(iu)
        basis_v_level.append(lv)
        basis_v_index.append(iv)
        basis_term_offset.append(len(term_parameter))
        basis_term_count.append(len(terms))
        for parameter, coefficient in terms:
            term_parameter.append(parameter)
            term_coefficient.append(coefficient)

    cell_offset = [x[0] for x in cells]
    cell_count = [x[1] for x in cells]

    with path.open("w") as f:
        f.write("#pragma once\n\n")
        f.write("#include <array>\n#include <cstddef>\n#include <cstdint>\n\n")
        f.write("namespace p0014_bspline_model {\n\n")
        f.write(f"// Generated from {source_name}.\n")
        f.write(f"// Raw refinement-tree controls: {raw_terms}.\n")
        f.write(f"// Exact merged field terms: {len(term_parameter)}.\n")
        f.write(f"// Unique spatial tensor B-splines: {len(keys)}.\n\n")
        f.write(f"inline constexpr std::size_t parameter_count = {PARAMETER_COUNT};\n")
        f.write(f"inline constexpr std::size_t u_base_knot_count = {U_BASE};\n")
        f.write(f"inline constexpr std::size_t v_base_segment_count = {V_BASE};\n")
        f.write(f"inline constexpr std::size_t u_max_level = {u_max};\n")
        f.write(f"inline constexpr std::size_t v_max_level = {v_max};\n")
        f.write(f"inline constexpr std::size_t dispatch_u_count = {DISPATCH_U};\n")
        f.write(f"inline constexpr std::size_t dispatch_v_count = {DISPATCH_V};\n")
        f.write(f"inline constexpr std::size_t spatial_basis_count = {len(keys)};\n")
        f.write(f"inline constexpr std::size_t field_term_count = {len(term_parameter)};\n")
        f.write(f"inline constexpr std::size_t cell_basis_reference_count = {len(refs)};\n\n")

        write_array(f, "std::uint16_t", "cell_offset", cell_offset)
        write_array(f, "std::uint16_t", "cell_count", cell_count)
        write_array(f, "std::uint16_t", "cell_basis", refs)

        write_array(f, "std::uint8_t", "basis_u_level", basis_u_level)
        write_array(f, "std::uint16_t", "basis_u_index", basis_u_index)
        write_array(f, "std::uint8_t", "basis_v_level", basis_v_level)
        write_array(f, "std::uint16_t", "basis_v_index", basis_v_index)
        write_array(f, "std::uint16_t", "basis_term_offset", basis_term_offset)
        write_array(f, "std::uint8_t", "basis_term_count", basis_term_count)

        write_array(f, "std::uint8_t", "term_parameter", term_parameter)
        write_array(
            f, "float", "term_coefficient", term_coefficient, per_line=6,
            formatter=lambda x: f"{np.float32(x).item():.9g}f")
        f.write("} // namespace p0014_bspline_model\n")

    return {
        "raw_tree_controls": raw_terms,
        "merged_field_terms": len(term_parameter),
        "spatial_basis_count": len(keys),
        "cell_basis_references": len(refs),
        "u_max_level": u_max,
        "v_max_level": v_max,
        "dispatch_cells": DISPATCH_U * DISPATCH_V,
        "candidate_min": min(x[1] for x in cells),
        "candidate_median": float(np.median([x[1] for x in cells])),
        "candidate_max": max(x[1] for x in cells),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", type=Path)
    ap.add_argument("output", type=Path)
    args = ap.parse_args()

    z, raw_terms, keys, basis_terms = merge_model(args.input)
    u_max, v_max, cells, refs = build_dispatch(keys)
    stats = emit_header(args.output, args.input.name, raw_terms, keys, basis_terms, u_max, v_max, cells, refs)
    for k, v in stats.items():
        print(f"{k}: {v}")


if __name__ == "__main__":
    main()
