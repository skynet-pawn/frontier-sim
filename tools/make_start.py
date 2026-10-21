#!/usr/bin/env python3
"""Pre-simulate a world and save it, so the viewer (and later the game) can open on a lived-in frontier.

    python tools/make_start.py DATA_DIR OUT.sav [--days N] [--seed S] [--cli PATH]

DATA_DIR is a data folder (data, data/slices/full, data/slices/full_4400). The run goes N simulated days (default 3650,
ten years from the scenario's start) and the whole state is written to OUT.sav. Open it with

    frontier_viewer --data DATA_DIR --load OUT.sav [--play "Kansas City"]

A snapshot loads only into the same data folder and the same build (the state version changes when a saved field is
added), so snapshots are made on demand and are not kept in the repository. A 31-node history takes seconds; the
4,694-node world takes about 25 minutes for ten years.
"""
import argparse
import os
import subprocess
import sys


def find_cli(explicit):
    if explicit:
        return explicit
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    for rel in ("build/cli/Release/frontier_cli.exe", "build/cli/frontier_cli.exe", "build/cli/frontier_cli",
                "build/frontier_cli"):
        path = os.path.join(root, rel)
        if os.path.exists(path):
            return path
    sys.exit("frontier_cli not found: build the project first or pass --cli PATH")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("data")
    ap.add_argument("out")
    ap.add_argument("--days", type=int, default=3650)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cli")
    a = ap.parse_args()
    cmd = [find_cli(a.cli), "--data", a.data, "--seed", str(a.seed), "--stop", str(a.days), "--quiet", "--save", a.out]
    print(" ".join(cmd))
    sys.exit(subprocess.call(cmd))


if __name__ == "__main__":
    main()
