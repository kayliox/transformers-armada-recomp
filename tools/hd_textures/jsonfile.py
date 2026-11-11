"""jsonfile.py - reading and writing the HD texture tools' own JSON files safely.

A file the tools keep (the colour-match ledger, the set's marks, the status, the options, the fonts' manifest) can be
left empty or half-written when the maker is stopped while it writes: a closed window, a graphics driver reset that took
the game and the maker with it (TODO 289), a full disk. A player's update then stopped at the HD textures every time,
on "json.decoder.JSONDecodeError: Expecting value: line 1 column 1 (char 0)" - an empty colour_gains.json (2026-11-11).

- read_json: a missing file is the default, silently; an empty or damaged one is the default too, with a warning line
  saying which file and that it is made again - never a crash.
- write_json: written to <file>.tmp, then put in place in one step (os.replace), so the file is always either the old
  one or the new one, never a part of one. A reader that has the file open a moment (the game) is waited for.
"""
import json
import os
import sys
import time


def read_json(path, default=None, what=None, expect=dict):
    """The file's JSON, or `default` when it is missing, empty, damaged or not of the type `expect` (None: any)."""
    path = os.fspath(path)
    try:
        with open(path, encoding="utf-8-sig") as fp:
            text = fp.read()
    except FileNotFoundError:
        return default
    except OSError as e:
        print(f"warning: {what or os.path.basename(path)} ({path}) could not be read ({e}): taken as missing", file=sys.stderr, flush=True)
        return default
    try:
        if not text.strip():
            raise ValueError("the file is empty")
        data = json.loads(text)
        if expect is not None and not isinstance(data, expect):
            raise ValueError(f"not a {expect.__name__}")
        return data
    except ValueError as e:
        print(f"warning: {what or os.path.basename(path)} ({path}) is damaged ({e}, probably cut off when the maker was "
              f"stopped while writing it): taken as missing and made again", file=sys.stderr, flush=True)
        return default


def replace_file(tmp, path, tries=20):
    """tmp put in place of path in one step; a reader holding the file a moment is waited for (up to ~2 s)."""
    for attempt in range(tries):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == tries - 1:
                raise
            time.sleep(0.1)


def write_text_atomic(path, text):
    """A text file written whole or not at all."""
    path = os.fspath(path)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fp:
        fp.write(text)
        fp.flush()
        os.fsync(fp.fileno())
    replace_file(tmp, path)


def write_json(path, data, **dump_args):
    """A JSON file written whole or not at all (json.dumps' arguments: indent, sort_keys, ...)."""
    write_text_atomic(path, json.dumps(data, **dump_args))
