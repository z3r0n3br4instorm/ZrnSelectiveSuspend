#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compares the frames of two zss-testapp runs.

Exact mode requires every byte to match, and is right when both runs used the
same driver. Tolerance mode is for runs that crossed drivers: it allows each
channel to differ by a small amount (rounding) and a tiny share of pixels to
differ by more (edges), and fails on anything larger.

Usage: compare_frames.py REFERENCE_DIR TEST_DIR [--tolerance N] [--max-outliers FRACTION]
"""
import argparse
import os
import sys


def compare(ref_dir, test_dir, tolerance=0, max_outliers=0.0):
    """Returns (ok, message). Compares every frame present in the reference."""
    names = sorted(n for n in os.listdir(ref_dir) if n.endswith(".rgba"))
    if not names:
        return False, f"no frames in {ref_dir}"
    worst = 0
    worst_outliers = 0.0
    for name in names:
        test_path = os.path.join(test_dir, name)
        if not os.path.exists(test_path):
            return False, f"{name} is missing from {test_dir}"
        with open(os.path.join(ref_dir, name), "rb") as f:
            a = f.read()
        with open(test_path, "rb") as f:
            b = f.read()
        if len(a) != len(b):
            return False, f"{name}: size {len(b)} differs from reference {len(a)}"
        if a == b:
            continue
        if tolerance == 0:
            first = next(i for i in range(len(a)) if a[i] != b[i])
            return False, f"{name}: differs from the reference at byte {first} (pixel {first // 4})"
        outliers = 0
        for i in range(0, len(a), 4):
            d = max(abs(a[i] - b[i]), abs(a[i + 1] - b[i + 1]), abs(a[i + 2] - b[i + 2]),
                    abs(a[i + 3] - b[i + 3]))
            if d > worst:
                worst = d
            if d > tolerance:
                outliers += 1
        share = outliers / (len(a) // 4)
        worst_outliers = max(worst_outliers, share)
        if share > max_outliers:
            return False, (f"{name}: {outliers} pixels ({share:.2%}) differ by more than "
                           f"{tolerance}; largest difference {worst}")
    if tolerance == 0:
        return True, f"{len(names)} frames identical"
    return True, (f"{len(names)} frames within tolerance {tolerance} "
                  f"(largest difference {worst}, worst outlier share {worst_outliers:.3%})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("test")
    ap.add_argument("--tolerance", type=int, default=0)
    ap.add_argument("--max-outliers", type=float, default=0.0)
    args = ap.parse_args()
    ok, message = compare(args.reference, args.test, args.tolerance, args.max_outliers)
    print(("ok: " if ok else "FAIL: ") + message)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
