#!/usr/bin/env python3
"""Run native decoder vectors through the pinned Kraken Rust reader as an oracle."""

import argparse
import pathlib
import shutil
import subprocess
import tempfile

PINNED_REVISION = "4b420d579bd381796d773875e36d14c9acaabf39"
ROOT = pathlib.Path(__file__).resolve().parents[1]
HARNESS = ROOT / "tests" / "fpkg_decoder_oracle" / "main.rs"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("reference", type=pathlib.Path)
    args = parser.parse_args()
    reference = args.reference.resolve()
    package = reference / "crates" / "kraken-mount-pkg"
    if not (package / "Cargo.toml").is_file():
        parser.error(f"{reference} is not a Kraken Mount source checkout")
    actual_revision = subprocess.check_output(
        ["git", "-C", str(reference), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual_revision != PINNED_REVISION:
        parser.error(
            f"reference revision mismatch: expected {PINNED_REVISION}, got {actual_revision}"
        )

    with tempfile.TemporaryDirectory(prefix="smp-fpkg-decoder-oracle-") as temporary:
        project = pathlib.Path(temporary)
        (project / "src").mkdir()
        manifest = f'''[package]\nname = "smp-fpkg-decoder-oracle"\nversion = "0.1.0"\nedition = "2021"\n\n[dependencies]\nkraken-mount-pkg = {{ path = "{package.as_posix()}" }}\n'''
        (project / "Cargo.toml").write_text(manifest, encoding="utf-8")
        shutil.copyfile(HARNESS, project / "src" / "main.rs")
        subprocess.run(
            ["cargo", "run", "--offline", "--quiet", "--manifest-path", str(project / "Cargo.toml")],
            check=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
