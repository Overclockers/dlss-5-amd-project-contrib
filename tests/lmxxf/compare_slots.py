"""Run correctness comparisons before timed split/inline sweeps on AMD hardware."""
import csv
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    exe, runtime, modules, output = sys.argv[1:5]
    performance_only = "--performance-only" in sys.argv[5:]
    output = Path(output)
    results = []

    def run(name, slots, args, passes=1):
        env = os.environ.copy()
        env["DLSS5_MULTI_PASS"] = str(passes)
        command = [exe, runtime, modules, str(slots), *args]
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=180)
        text = result.stdout + result.stderr
        (output / (name + ".log")).write_text(text, encoding="utf-8")
        if result.returncode:
            raise RuntimeError(f"{name}: exit {result.returncode}\n{text[-4000:]}")
        hashes = re.findall(r"^HASH .+$", text, re.M)
        rows = re.findall(r"^BENCH (.+)$", text, re.M)
        row = rows[-1] if rows else None
        if row is None:
            raise RuntimeError(f"{name}: missing benchmark summary")
        fields = dict(token.split("=", 1) for token in row.split())
        fields.update(name=name, passes=passes)
        print(name, row, flush=True)
        if "--sweep" in args:
            return hashes, [dict(dict(token.split("=", 1) for token in row.split()), name=name, passes=passes) for row in rows]
        return hashes, fields

    # Changing colour reuses the same game texture. Auto exposure, History and
    # first-pass results must remain identical to split, including three passes.
    for profile, flags, passes in ([] if performance_only else [
        ("spatial", [], 1),
        ("history-exposure-first", ["--history", "--auto", "--first"], 3),
    ]):
        reference = None
        for slots in range(6):
            hashes, _ = run(f"verify-{profile}-{slots}", slots,
                            ["--verify-ahead", "--debug", "--frames=9", *flags], passes)
            if len(hashes) != 9:
                raise RuntimeError("Missing verification frame hashes")
            if reference is None:
                reference = hashes
            elif hashes != reference:
                raise RuntimeError(f"{profile}: inline {slots} output differs from split")
    if not performance_only:
        run("hot-limits-and-split", 3, ["--hot", "--debug", "--history", "--frames=60", "--wait=0"])

    # Forward/reverse order reduces run-order/thermal bias. Shader/debug/readback
    # validation is excluded from these timings; every process warms up first.
    for profile, flags, passes in [
        ("720-spatial-unpaced", ["--height=720"], 1),
        ("1080-history-unpaced", ["--height=1080", "--history"], 1),
        ("1080-history-60hz", ["--height=1080", "--history", "--interval=16.667"], 1),
    ]:
        _, rows = run(f"perf-{profile}-same-session", 0, ["--sweep", "--frames=90", *flags], passes)
        if len(rows) != 12:
            raise RuntimeError("Incomplete same-session forward/reverse sweep")
        for row in rows:
            row.update(profile=profile)
            results.append(row)
        (output / "slots-results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    with (output / "slots-results.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=list(results[0]))
        writer.writeheader()
        writer.writerows(results)
    print("SLOTS_PERFORMANCE_PASS" if performance_only else "SLOTS_COMPARISON_PASS: split == inline 1..5; timed sweeps saved", flush=True)


if __name__ == "__main__":
    main()
