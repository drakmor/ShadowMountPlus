#!/usr/bin/env python3
"""Check stale sandbox mount recovery and identity guards without a console."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
from test_storage_paths import functions


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / "src/sm_fakelib.c").read_text(encoding="utf-8")
    layer = re.search(r"typedef struct \{[^}]+\} fakelib_layer_t;", source)
    if not layer:
        raise RuntimeError("Cannot find sandbox layer type")
    with tempfile.TemporaryDirectory(prefix="sm-sandbox-cleanup-") as directory:
        build = Path(directory)
        (build / "sandbox_layer.inc").write_text(layer.group(0), encoding="utf-8")
        (build / "sandbox_cleanup.inc").write_text(functions(root, "src/sm_fakelib.c", [
            "sandbox_mount_matches", "sandbox_mount_identity_present",
            "unmount_unreachable_sandbox_layer", "unmount_sandbox_layer"]), encoding="utf-8")
        binary = build / "cleanup"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-I" + str(root / "include"), "-I" + str(build),
            str(root / "tests/test_sandbox_cleanup.c"), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
