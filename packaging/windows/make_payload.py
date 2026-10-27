#!/usr/bin/env python
"""Pack a staged Aurora directory into the installer's ZIP payload.

The custom setup reader supports stored and DEFLATE entries without ZIP64, so
this script refuses archives that would need ZIP64 and writes paths relative to
the package root (no "./" prefix, forward slashes).
"""

import os
import sys
import zipfile


def fail(message: str) -> "NoReturn":
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: make_payload.py <package_dir> <out.zip>", file=sys.stderr)
        return 2
    root = os.path.abspath(sys.argv[1])
    output = os.path.abspath(sys.argv[2])
    if not os.path.isdir(root):
        fail(f"package directory not found: {root}")

    files: list[tuple[str, str]] = []
    for directory, subdirs, names in os.walk(root):
        subdirs.sort()
        names.sort()
        for name in names:
            full = os.path.join(directory, name)
            if os.path.islink(full):
                fail(f"package contains a symbolic link: {full}")
            relative = os.path.relpath(full, root)
            if relative.startswith(".."):
                fail(f"package path escapes the root: {full}")
            arcname = relative.replace(os.sep, "/")
            files.append((full, arcname))

    names = {name.lower() for _, name in files}
    for required in ("aurora_browser.exe", "aurora_bootstrap.exe",
                     "aurora_build.manifest"):
        if required not in names:
            fail(f"package is missing {required}")

    os.makedirs(os.path.dirname(output) or ".", exist_ok=True)
    try:
        with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED,
                             compresslevel=9, allowZip64=False) as archive:
            for full, arcname in files:
                archive.write(full, arcname=arcname)
    except (zipfile.LargeZipFile, OSError) as error:
        fail(str(error))

    with zipfile.ZipFile(output) as archive:
        broken = archive.testzip()
        if broken is not None:
            fail(f"ZIP CRC check failed for {broken}")
        count = len(archive.namelist())
    print(f"Payload: {count} files, {os.path.getsize(output)} bytes -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
