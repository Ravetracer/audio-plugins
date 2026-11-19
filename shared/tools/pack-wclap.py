#!/usr/bin/env python3
"""Packs a WCLAP bundle directory into the .wclap.tar.gz that hosts import
(openDAW: Import WebCLAP). One top directory named like the bundle, plain
ustar entries, sorted, fixed times and owners: the same bundle always gives
the same bytes.

    tools/pack-wclap.py <bundle dir> [output]   (default: <bundle dir>.tar.gz)
"""
import gzip
import io
import os
import sys
import tarfile


def pack(bundle, output):
    bundle = os.path.normpath(bundle)
    root = os.path.basename(bundle)
    paths = []
    for directory, dirs, files in os.walk(bundle):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        paths += [os.path.join(directory, f) for f in sorted(files) if not f.startswith(".")]
    if not any(os.path.basename(p) == "module.wasm" for p in paths):
        sys.exit(f"{bundle}: no module.wasm")
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        for path in paths:
            info = tarfile.TarInfo(root + "/" + os.path.relpath(path, bundle).replace(os.sep, "/"))
            info.size = os.path.getsize(path)
            info.mode = 0o644
            info.mtime = 0
            info.uname = info.gname = ""
            with open(path, "rb") as f:
                tar.addfile(info, f)
    with open(output, "wb") as out, gzip.GzipFile(filename="", mode="wb", fileobj=out, compresslevel=9, mtime=0) as gz:
        gz.write(buffer.getvalue())
    print(f"{output}: {len(paths)} files, {os.path.getsize(output) / 1048576:.2f} MB")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pack(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else os.path.normpath(sys.argv[1]) + ".tar.gz")
