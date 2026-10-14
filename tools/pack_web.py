#!/usr/bin/env python3
"""Packs an embedded web asset for the firmware image.

The configuration page is stored in flash compressed and served with
"Content-Encoding: gzip", which browsers decode transparently. That keeps the
page inside the application image — it survives a formatted filesystem and
cannot be deleted from it — while costing only a few kilobytes of the app
partition.

Usage: pack_web.py <input.html> <output.gz>
"""
import gzip
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    data = src.read_bytes()
    # mtime=0 keeps the output reproducible, so an unchanged page produces an
    # unchanged object file and nothing gets relinked.
    packed = gzip.compress(data, 9, mtime=0)
    dst.write_bytes(packed)
    print(f"pack_web: {src.name} {len(data)} B -> {dst.name} {len(packed)} B "
          f"({100 * len(packed) // max(1, len(data))}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
