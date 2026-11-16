#!/usr/bin/env python3
"""Check that game executables gain only their missing execute bits."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="sm-exec-permissions-") as directory:
        binary = Path(directory) / "exec_permissions"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-I" + str(root / "include"),
            str(root / "tests/test_exec_permissions.c"), "-o", str(binary)]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
