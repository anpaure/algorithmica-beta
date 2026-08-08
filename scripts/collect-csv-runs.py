#!/usr/bin/env python3
"""Run a one-row CSV benchmark repeatedly and retain every process result.

This complements median-csv.py for uncertainty plots. The command must emit
one header and one data row per process; the output gains a leading `run`
column so the complete set of process-level measurements is reproducible.
"""

import argparse
import csv
import io
from pathlib import Path
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if args.runs <= 0:
        parser.error("--runs must be positive")
    if not command:
        parser.error("a benchmark command is required after --")

    header = None
    output_rows = []
    for run in range(1, args.runs + 1):
        completed = subprocess.run(
            command, check=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True,
        )
        if completed.stderr:
            print(f"run {run}: {completed.stderr.strip()}", file=sys.stderr)
        rows = list(csv.reader(io.StringIO(completed.stdout)))
        if len(rows) != 2 or not rows[0] or len(rows[0]) != len(rows[1]):
            raise ValueError("benchmark must emit exactly one rectangular CSV row")
        if header is None:
            header = rows[0]
        elif rows[0] != header:
            raise ValueError("CSV header changed between runs")
        output_rows.append([str(run), *rows[1]])

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["run", *header])
        writer.writerows(output_rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
