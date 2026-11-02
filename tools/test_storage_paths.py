#!/usr/bin/env python3
"""Check USB suspend classification and M.2 destinations without a console."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


def functions(root, source, names):
    text = (root / source).read_text(encoding="utf-8")
    parts = []
    for name in names:
        match = re.search(r"^[\w \t*]+\b" + re.escape(name) +
                          r"\([^;{]*\)\s*\{.*?^\}", text, re.M | re.S)
        if not match:
            raise RuntimeError(f"Cannot find {source}:{name}")
        parts.append(match.group(0))
    return "\n\n".join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="sm-storage-paths-") as directory:
        build = Path(directory)
        helpers = functions(root, "src/sm_path_utils.c", [
            "is_under_image_mount_base", "path_matches_root_or_child",
            "find_usb_storage_root", "is_usb_storage_path", "usb_storage_root_mounted"])
        helpers += "\n" + functions(root, "src/sm_game_lifecycle.c", ["title_is_usb_backed"])
        helpers += "\n" + functions(root, "src/sm_api_service.c", ["destination_is_managed"])
        (build / "storage_path_helpers.inc").write_text(helpers, encoding="utf-8")
        binary = build / "storage_paths"
        command = shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-I" + str(root / "include"), "-I" + str(build),
            str(root / "tests/test_storage_paths.c"), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
