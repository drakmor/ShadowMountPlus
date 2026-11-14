#!/usr/bin/env python3
"""Compare native C Kraken decoding with the pinned Kraken Mount oracle."""

import argparse
import os
import pathlib
import shutil
import struct
import subprocess
import tempfile

PINNED_REVISION = "4b420d579bd381796d773875e36d14c9acaabf39"
ROOT = pathlib.Path(__file__).resolve().parents[1]
ORACLE_C = ROOT / "tests" / "fpkg_decoder_oracle" / "decoder_diff.c"
ORACLE_RS = ROOT / "tests" / "fpkg_decoder_oracle" / "decoder_diff.rs"
MAX_INPUT = 0x40000


def next_u32(state: int) -> tuple[int, int]:
    value = state
    value = (value ^ (value << 13)) & 0xFFFFFFFF
    value = (value ^ (value >> 17)) & 0xFFFFFFFF
    value = (value ^ (value << 5)) & 0xFFFFFFFF
    return value, value


def corpus() -> tuple[bytes, list[int]]:
    state = 0x6D2B79F5
    encoded = bytearray()
    output_sizes: list[int] = []
    cases: list[tuple[bytes, int, int, int]] = []

    for iteration in range(16384):
        state, value = next_u32(state)
        stored_size = 1 + value % 128
        stored = bytearray()
        for _ in range(stored_size):
            state, value = next_u32(state)
            stored.append(value & 0xFF)

        if iteration & 3 == 0:
            stored[0] &= 0x7F
        elif iteration & 3 == 1:
            stored[0] = 0x80 | (stored[0] & 0x0F)
        elif iteration & 3 == 2:
            stored[0] = 0x20 | (stored[0] & 0x1F)
        else:
            stored[0] = 0xA0 | (stored[0] & 0x0F)

        state, value = next_u32(state)
        output_size = 1 + value % 512
        even_length = stored_size
        if iteration & 7 == 7:
            state, value = next_u32(state)
            even_length = 1 + value % stored_size
        state, value = next_u32(state)
        mode = value & 0xFF
        cases.append((bytes(stored), output_size, even_length, mode))

    # Exercise the decoder's full u8 mode behavior and the reference's sparse
    # zero-block behavior, which are not reachable from a five-bit FPKG record.
    cases.append((b"", 64, 0, 0))
    cases.extend((b"", 64, 0, mode) for mode in (1, 2, 0x20, 0x80))
    for output_size in (1, 7, 8, 0x1FFFF, 0x20000, 0x20001, MAX_INPUT):
        stored = bytes((index * 29 + index // 113) & 0xFF
                       for index in range(output_size))
        even_length = min(output_size, 0x20000)
        cases.append((stored, output_size, even_length, 0))
        for mode in (0x20, 0x80):
            cases.append((stored, output_size, even_length, mode))

    for stored, output_size, even_length, mode in cases:
        encoded.extend(struct.pack("<IIIB", len(stored), output_size,
                                   even_length, mode))
        encoded.extend(stored)
        output_sizes.append(output_size)
    return bytes(encoded), output_sizes


def parse_transcript(blob: bytes, output_sizes: list[int], source: str):
    results = []
    at = 0
    for case, output_size in enumerate(output_sizes):
        if at >= len(blob) or blob[at] not in (0, 1):
            raise RuntimeError(f"{source}: malformed result for case {case}")
        accepted = bool(blob[at])
        at += 1
        output = None
        if accepted:
            end = at + output_size
            if end > len(blob):
                raise RuntimeError(f"{source}: truncated output for case {case}")
            output = blob[at:end]
            at = end
        results.append((accepted, output))
    if at != len(blob):
        raise RuntimeError(f"{source}: trailing result bytes ({len(blob) - at})")
    return results


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

    clang = shutil.which("clang")
    cargo = shutil.which("cargo")
    if not clang or not cargo:
        parser.error("clang and cargo are required")

    input_blob, output_sizes = corpus()
    with tempfile.TemporaryDirectory(prefix="smp-fpkg-decoder-diff-") as temporary:
        work = pathlib.Path(temporary)
        c_executable = work / "decoder-c"
        subprocess.run(
            [clang, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
             "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
             "-I", str(ROOT / "include"), str(ORACLE_C),
             str(ROOT / "src" / "sm_fpkg_decoder.c"), "-o", str(c_executable)],
            check=True,
        )

        project = work / "oracle"
        (project / "src").mkdir(parents=True)
        (project / "src" / "main.rs").write_text(
            ORACLE_RS.read_text(encoding="utf-8"), encoding="utf-8"
        )
        (project / "Cargo.toml").write_text(
            "[package]\nname = \"smp-fpkg-decoder-diff\"\n"
            "version = \"0.1.0\"\nedition = \"2021\"\n\n"
            f"[dependencies]\nkraken-mount-pkg = {{ path = \"{package.as_posix()}\" }}\n",
            encoding="utf-8",
        )
        c_result = subprocess.run(
            [str(c_executable)], input=input_blob, stdout=subprocess.PIPE,
            check=True,
        ).stdout
        rust_env = os.environ.copy()
        rust_env["CARGO_TARGET_DIR"] = str(work / "cargo-target")
        rust_result = subprocess.run(
            [cargo, "run", "--offline", "--quiet", "--manifest-path",
             str(project / "Cargo.toml")], input=input_blob,
            stdout=subprocess.PIPE, check=True, env=rust_env,
        ).stdout

    c_cases = parse_transcript(c_result, output_sizes, "native C")
    rust_cases = parse_transcript(rust_result, output_sizes, "pinned Kraken")
    for index, (c_case, rust_case) in enumerate(zip(c_cases, rust_cases)):
        if c_case[0] != rust_case[0]:
            raise SystemExit(
                f"decoder acceptance differs at case {index}: C={c_case[0]} "
                f"Kraken={rust_case[0]}"
            )
        if c_case[0] and c_case[1] != rust_case[1]:
            differing = next(
                offset for offset, pair in enumerate(zip(c_case[1], rust_case[1]))
                if pair[0] != pair[1]
            )
            raise SystemExit(
                f"decoder output differs at case {index}, byte {differing}"
            )
    print(
        f"Pinned Kraken decoder differential: {len(c_cases)} cases "
        "match acceptance and decoded bytes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
