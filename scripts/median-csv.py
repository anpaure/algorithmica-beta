#!/usr/bin/env python3
"""Run a CSV-emitting benchmark repeatedly and take cell-wise medians.

The benchmark must emit the same header and rows in the same order on every
run. Cells that are identical are copied verbatim. Cells that differ must be
numeric; for an odd run count, the script writes the original spelling of the
median sample.

Example:
    python3 scripts/median-csv.py --runs 5 --output results.txt -- ./bench bench
"""

import argparse
import csv
from decimal import Decimal, InvalidOperation
import io
from pathlib import Path
import subprocess
import sys


def parse_csv(output: str) -> list[list[str]]:
    rows = list(csv.reader(io.StringIO(output)))
    if not rows:
        raise ValueError("benchmark emitted no CSV rows")
    width = len(rows[0])
    if width == 0 or any(len(row) != width for row in rows):
        raise ValueError("benchmark emitted a malformed CSV table")
    return rows


def median_cell(values: list[str]) -> str:
    if all(value == values[0] for value in values[1:]):
        return values[0]

    numeric = []
    try:
        for value in values:
            numeric.append((Decimal(value), value))
    except InvalidOperation as error:
        raise ValueError(
            f"non-numeric cell changed between runs: {values}"
        ) from error

    numeric.sort(key=lambda item: item[0])
    return numeric[len(numeric) // 2][1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    if args.runs <= 0 or args.runs % 2 == 0:
        parser.error("--runs must be a positive odd number")
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a benchmark command is required after --")

    tables: list[list[list[str]]] = []
    for run in range(args.runs):
        completed = subprocess.run(
            command,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.stderr:
            print(f"run {run + 1}: {completed.stderr.strip()}", file=sys.stderr)
        tables.append(parse_csv(completed.stdout))

    reference = tables[0]
    for table in tables[1:]:
        if len(table) != len(reference) or table[0] != reference[0]:
            raise ValueError("CSV shape or header changed between runs")

    result = [reference[0]]
    for row_index in range(1, len(reference)):
        row = []
        for column in range(len(reference[0])):
            row.append(median_cell([
                table[row_index][column] for table in tables
            ]))
        result.append(row)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as handle:
        csv.writer(handle, lineterminator="\n").writerows(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
