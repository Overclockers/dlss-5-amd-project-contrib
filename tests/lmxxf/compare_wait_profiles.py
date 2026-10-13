"""Compare bounded wait schedules against split on the same device/session.

Each backend first checks cold-process, ahead-frame output hashes, then measures
old/new polling budgets in forward/reverse order without debug or readbacks.
This is synthetic NR throughput, not a prediction of game FPS.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    exe, runtime, assets, output = sys.argv[1:5]
    backend = sys.argv[5:]
    mochi = "--mochizuki" in backend
    if any(arg not in ("--mochizuki", "--aco") for arg in backend):
        raise ValueError("Unsupported backend flag")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env.update(DLSS5_NETWORK_FREE_RES="1", DLSS5_FIT_LARGE="1",
               DLSS5_FAST_NUMERIC="1", DLSS5_SKIP_BLOCKS="none")
    results = []

    def run(name, slots, args, passes):
        env["DLSS5_MULTI_PASS"] = str(passes)
        command = [exe, runtime, assets, str(slots), *backend, "--height=1440", "--history", *args]
        log = output / (name + ".log")
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=600)
        text = log.read_text(encoding="utf-8", errors="replace")
        if result.returncode:
            raise RuntimeError(f"{name}: exit {result.returncode}\n{text[-4000:]}")
        rows = [dict(token.split("=", 1) for token in row.split())
                for row in re.findall(r"^BENCH (.+)$", text, re.M)]
        # Keep completed measurements even when a regression rejects the run.
        log.with_suffix(".rows.json").write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
        if not rows:
            raise RuntimeError(f"{name}: no timing rows")
        for row in rows:
            if sum(int(row[k]) for k in ("applied", "timeout", "skipped")) != int(row["frames"]):
                raise RuntimeError(f"{name}: missing frames: {row}")
            if int(row["timeout"]):
                raise RuntimeError(f"{name}: unexpected timeout: {row}")
        return re.findall(r"^HASH .+$", text, re.M), rows

    reference = None
    for slots in range(5 if mochi else 6):
        flags = ["--verify-ahead", "--frames=9"]
        if not mochi:
            flags.extend(["--auto", "--first"])
        hashes, rows = run(f"verify-{slots}", slots, flags, 1 if mochi else 3)
        if len(hashes) != 9 or (reference is not None and hashes != reference):
            raise RuntimeError(f"inline {slots}: incomplete or different final/first hashes")
        reference = hashes
        print(f"EXACT_HASH_PASS slots={slots}", flush=True)
    for name, flags in [("unpaced", []), ("60hz", ["--interval=16.667"])]:
        _, rows = run(name, 0, ["--wait-sweep", "--frames=180", *flags], 1)
        if len(rows) != (18 if mochi else 22):
            raise RuntimeError("Incomplete forward/reverse wait sweep")
        for row in rows:
            if name == "unpaced" and int(row["skipped"]):
                raise RuntimeError(f"Unpaced frame unexpectedly skipped: {row}")
            row["profile"] = name
            print("BENCH " + " ".join(f"{k}={v}" for k, v in row.items()), flush=True)
        results.extend(rows)
        (output / "wait-results.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print("WAIT_COMPARISON_PASS", flush=True)


if __name__ == "__main__":
    main()
