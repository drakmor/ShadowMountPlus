#!/usr/bin/env python3
"""Verify that generated ShellCore hook prologues are safe to copy."""

from __future__ import annotations

import argparse
from pathlib import Path

from capstone import CS_ARCH_X86, CS_GRP_CALL, CS_GRP_JUMP, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

from generate_shellcore_offsets import (
    ShellCore,
    firmware_key,
)
from generate_shellcore_offsets_from_files import firmware_files, generate


def verify_firmware(path: Path, firmware: int) -> None:
    shellcore = ShellCore(path)
    targets = shellcore.locate_targets()
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True

    names = ["launch_app"]
    # AppInstallAll is hooked after the public TitleDir RPC disappeared.
    if firmware >= 0x1200:
        names.append("install_all")

    for name in names:
        address = targets[name]
        patch_size = shellcore.patch_size(address)
        offset = shellcore.virtual_to_file(address)
        code = shellcore.data[offset : offset + patch_size]
        instructions = list(decoder.disasm(code, address))
        if sum(instruction.size for instruction in instructions) != patch_size:
            raise ValueError(f"{path}: {name}: split instruction")
        for instruction in instructions:
            if instruction.group(CS_GRP_CALL) or instruction.group(CS_GRP_JUMP):
                raise ValueError(
                    f"{path}: {name}: control flow in copied prologue at "
                    f"0x{instruction.address:x}"
                )
            for operand in instruction.operands:
                if operand.type == X86_OP_MEM and operand.mem.base == X86_REG_RIP:
                    raise ValueError(
                        f"{path}: {name}: RIP-relative operand at "
                        f"0x{instruction.address:x}"
                    )

    sandbox_address = targets["sandbox_ready"]
    sandbox_offset = shellcore.virtual_to_file(sandbox_address)
    sandbox_instruction = next(
        decoder.disasm(shellcore.data[sandbox_offset : sandbox_offset + 5],
                       sandbox_address),
        None,
    )
    if (
        sandbox_instruction is None
        or sandbox_instruction.mnemonic != "call"
        or sandbox_instruction.size != 5
        or not sandbox_instruction.operands
        or sandbox_instruction.operands[0].type != X86_OP_IMM
        or sandbox_instruction.operands[0].imm
        != targets["sandbox_ready_target"]
    ):
        raise ValueError(
            f"{path}: invalid sceApplicationSpawn2 call at "
            f"0x{sandbox_address:x}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    firmware_inputs: list[tuple[int, Path]] = []
    firmware_dirs = sorted(
        path
        for path in args.root.iterdir()
        if (path / "system/vsh/SceShellCore.elf").is_file()
    )
    for firmware_dir in firmware_dirs:
        firmware_inputs.append(
            (
                firmware_key(firmware_dir.name),
                firmware_dir / "system/vsh/SceShellCore.elf",
            )
        )
    for major, minor, path in firmware_files(args.root):
        firmware_inputs.append((firmware_key(f"{major}.{minor:02d}"), path))
    if not firmware_inputs:
        raise ValueError(f"no SceShellCore firmware files under {args.root}")

    for firmware, path in firmware_inputs:
        verify_firmware(path, firmware)

    checked_in_offsets = (
        Path(__file__).resolve().parents[1] / "src/sm_shellcore_offsets.inc"
    )
    if not firmware_dirs:
        expected = generate(args.root)
        actual = checked_in_offsets.read_text(encoding="utf-8")
        if actual != expected:
            raise ValueError(
                f"{checked_in_offsets}: generated offsets are stale for "
                f"{args.root}"
            )

    print(
        f"verified {len(firmware_inputs)} firmware trampoline layouts"
        + (" and checked-in offset coverage" if not firmware_dirs else "")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
