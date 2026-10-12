#!/usr/bin/env python3
import sys

original = open(sys.argv[1], "rb").read()
patched = open(sys.argv[2], "rb").read()

if len(original) != len(patched):
    print(f"Warning: file sizes differ: {len(original)} vs {len(patched)}")

for offset, (a, b) in enumerate(zip(original, patched)):
    if a != b:
        print(f"{offset:08X}: {a:02X} -> {b:02X}")