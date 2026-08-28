#!/usr/bin/env python3

import collections
import sys


def fail(message):
    raise AssertionError(message)


def location(value):
    line, column = value.split(".", 1)
    return int(line), int(column)


def read_report(path):
    functions = []
    source_blocks = []
    current_file = None
    current_file_block = None
    current_function = None
    with open(path, encoding="utf-8") as report:
        header = report.readline().rstrip("\n").split("\t")
        if len(header) < 4 or header[:2] != ["txtcvrg", "4"]:
            fail(f"{path}: not a txtcvrg v4 report")
        if "view=segments" not in header:
            fail(f"{path}: not a segment report")

        for line_number, line in enumerate(report, 2):
            fields = line.rstrip("\n").split("\t")
            if not fields or fields[0] in ("", "end"):
                continue
            if fields[0] == "file":
                if len(fields) != 2:
                    fail(f"{path}:{line_number}: malformed file record")
                current_file = fields[1]
                current_function = None
                current_file_block = {"file": current_file, "regions": []}
                source_blocks.append(current_file_block)
                continue
            if fields[0] == "function":
                if current_file is None or len(fields) != 5:
                    fail(f"{path}:{line_number}: malformed function record")
                current_function = {
                    "file": current_file,
                    "name": fields[1],
                    "total": int(fields[2]),
                    "regions": [],
                }
                functions.append(current_function)
                continue
            if fields[0] not in ("1", "2"):
                continue
            if len(fields) != 4:
                fail(f"{path}:{line_number}: malformed region record")
            start = location(fields[1])
            end = location(fields[2])
            if start > end:
                fail(f"{path}:{line_number}: reversed region")
            if current_function is None:
                # Macro-body regions retain the version 3 source-owned layout.
                current_file_block["regions"].append((fields[0], start, end))
                continue
            # Counts are deliberately excluded from the structural key.
            current_function["regions"].append((fields[0], start, end))

    for function in functions:
        code_regions = [
            (start, end)
            for kind, start, end in function["regions"]
            if kind == "1"
        ]
        if len(code_regions) != function["total"]:
            fail(f"{path}: incorrect segment total for {function['name']}")
        previous_end = None
        for start, end in code_regions:
            # A function with no source-width executable range can retain a
            # zero-length counter point so its hit state is not lost.
            if start == end:
                continue
            if previous_end is not None and previous_end > start:
                fail(f"{path}: overlapping segments for {function['name']}")
            previous_end = end
    for block in source_blocks:
        code_regions = [
            (start, end)
            for kind, start, end in block["regions"]
            if kind == "1"
        ]
        previous_end = None
        for start, end in code_regions:
            if start == end:
                continue
            if previous_end is not None and previous_end > start:
                fail(f"{path}: overlapping source-owned segments")
            previous_end = end
    return functions


def topology(function):
    return function["total"], function["regions"]


def main():
    if len(sys.argv) != 3:
        fail("usage: check_txtcvrg_segments.py BASELINE EXECUTION")
    baseline = read_report(sys.argv[1])
    execution = read_report(sys.argv[2])
    if not execution:
        fail("execution report contains no functions")

    baseline_by_name = collections.defaultdict(list)
    for function in baseline:
        baseline_by_name[(function["file"], function["name"])].append(
            topology(function)
        )
    for function in execution:
        candidates = baseline_by_name[(function["file"], function["name"])]
        if topology(function) not in candidates:
            fail(
                "baseline/execution topology differs for "
                f"{function['file']}:{function['name']}"
            )

if __name__ == "__main__":
    main()
