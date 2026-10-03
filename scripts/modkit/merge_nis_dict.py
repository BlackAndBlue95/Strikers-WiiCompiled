#!/usr/bin/env python3
"""Appends cutscene dictionary entries to Mario Strikers Charged's Art/nis/nis_dict.txt.

    merge_nis_dict.py <game nis_dict.txt> <entries.txt> <out nis_dict.txt>

The game's file keeps its CRLF lines (trailing NULs and spaces dropped); each line of the entries
file follows it, CRLF-terminated (a final newline leaves a blank line, as the game's own file has).
"""

import sys


def main(game, entries, out):
    with open(game, "rb") as f:
        merged = f.read().rstrip(b"\0 ")
    if merged and not merged.endswith(b"\n"):
        merged += b"\r\n"
    added = 0
    with open(entries, "rb") as f:
        for line in f.read().split(b"\n"):
            merged += line.rstrip(b"\r") + b"\r\n"
            added += 1
    with open(out, "wb") as f:
        f.write(merged)
    print("%s: + %d line(s)" % (out, added))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
