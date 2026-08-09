#!/usr/bin/env python3
"""Generate SceShellCore bridge offsets from unpacked firmware files.

The generator identifies functions through semantic log strings, finds the
containing function prologue and emits a compact firmware offset table.  It is
intentionally independent from IDA databases so the checked-in table can be
reproduced from unpacked firmware files.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_64, Cs

PROLOGUE = b"\x55\x48\x89\xe5"
TARGET_NAMES = (
    "launch_app",
    "install_title_dir",
    "install_all",
)


@dataclass
class LoadSegment:
    file_offset: int
    virtual_address: int
    file_size: int
    flags: int


class ShellCore:
    def __init__(self, path: Path):
        self.path = path
        self.data = path.read_bytes()
        header = struct.unpack_from("<16sHHIQQQIHHHHHH", self.data, 0)
        phoff, phentsize, phnum = header[5], header[9], header[10]
        self.loads: list[LoadSegment] = []
        self.executable: LoadSegment | None = None
        for index in range(phnum):
            values = struct.unpack_from(
                "<IIQQQQQQ", self.data, phoff + index * phentsize
            )
            p_type, flags, offset, address, _, file_size, _, _ = values
            if p_type != 1:
                continue
            segment = LoadSegment(offset, address, file_size, flags)
            self.loads.append(segment)
            if flags & 1:
                self.executable = segment
        if self.executable is None:
            raise ValueError(f"{path}: executable PT_LOAD not found")
        self._rip_references: list[tuple[int, int]] | None = None

    def file_to_virtual(self, offset: int) -> int:
        for segment in self.loads:
            relative = offset - segment.file_offset
            if 0 <= relative < segment.file_size:
                return segment.virtual_address + relative
        raise ValueError(f"file offset 0x{offset:x} is not mapped")

    def virtual_to_file(self, address: int) -> int:
        for segment in self.loads:
            relative = address - segment.virtual_address
            if 0 <= relative < segment.file_size:
                return segment.file_offset + relative
        raise ValueError(f"virtual address 0x{address:x} is not mapped")

    def string_addresses(self, needle: bytes, use_string_start: bool) -> list[int]:
        result: list[int] = []
        offset = 0
        while True:
            offset = self.data.find(needle, offset)
            if offset < 0:
                return result
            string_offset = offset
            if use_string_start:
                string_offset = self.data.rfind(
                    b"\0", max(0, offset - 256), offset
                ) + 1
            result.append(self.file_to_virtual(string_offset))
            offset += 1

    def rip_xrefs(self, targets: list[int], slop: int = 0) -> list[int]:
        if self._rip_references is None:
            segment = self.executable
            assert segment is not None
            code = self.data[
                segment.file_offset : segment.file_offset + segment.file_size
            ]
            references: list[tuple[int, int]] = []
            for index in range(len(code) - 7):
                if code[index] not in (0x48, 0x4C):
                    continue
                if code[index + 1] not in (0x8D, 0x8B):
                    continue
                if code[index + 2] & 0xC7 != 0x05:
                    continue
                displacement = struct.unpack_from("<i", code, index + 3)[0]
                instruction_address = segment.virtual_address + index
                references.append(
                    (instruction_address, instruction_address + 7 + displacement)
                )
            self._rip_references = references

        if slop == 0:
            exact_targets = set(targets)
            return [
                address
                for address, destination in self._rip_references
                if destination in exact_targets
            ]
        return [
            address
            for address, destination in self._rip_references
            if any(
                target - slop <= destination <= target + slop
                for target in targets
            )
        ]

    def preceding_prologue(self, address: int, window: int = 0x40000) -> int:
        end = self.virtual_to_file(address)
        start = max(self.executable.file_offset, end - window)  # type: ignore[union-attr]
        found = self.data.rfind(PROLOGUE, start, end)
        if found < 0:
            raise ValueError(f"no prologue before 0x{address:x}")
        return self.file_to_virtual(found)

    def locate_targets(self) -> dict[str, int]:
        launch_strings = self.string_addresses(b"launchApp(%s)", True)
        launch_xrefs = self.rip_xrefs(launch_strings)
        if len(launch_xrefs) != 1:
            raise ValueError(f"launchApp xrefs: {launch_xrefs!r}")
        launch = self.preceding_prologue(launch_xrefs[0])

        install_strings = self.string_addresses(b"AppInstallTitleDirMain", False)
        install_groups: dict[int, int] = {}
        for xref in self.rip_xrefs(install_strings, slop=8):
            prologue = self.preceding_prologue(xref)
            install_groups[prologue] = install_groups.get(prologue, 0) + 1
        if not install_groups:
            raise ValueError("AppInstallTitleDirMain xrefs not found")
        best_count = max(install_groups.values())
        best = [address for address, count in install_groups.items() if count == best_count]
        if len(best) != 1:
            raise ValueError(f"ambiguous AppInstallTitleDirMain: {install_groups!r}")

        app_install_all_strings = self.string_addresses(b"AppInstallAll()", True)
        app_install_all_xrefs = self.rip_xrefs(app_install_all_strings)
        call_targets: dict[int, int] = {}
        for xref in app_install_all_xrefs:
            xref_file = self.virtual_to_file(xref)
            for call_file in range(xref_file - 32, xref_file):
                if self.data[call_file] != 0xE8:
                    continue
                call_address = self.file_to_virtual(call_file)
                displacement = struct.unpack_from("<i", self.data, call_file + 1)[0]
                target = call_address + 5 + displacement
                call_targets[target] = call_targets.get(target, 0) + 1
        if not call_targets:
            raise ValueError("AppInstallAll call target not found")
        app_install_all = max(call_targets, key=call_targets.get)

        return {
            "launch_app": launch,
            "install_title_dir": best[0],
            "install_all": app_install_all,
        }

    def patch_size(self, address: int, minimum: int = 12) -> int:
        offset = self.virtual_to_file(address)
        decoder = Cs(CS_ARCH_X86, CS_MODE_64)
        size = 0
        for instruction in decoder.disasm(self.data[offset : offset + 64], address):
            size += instruction.size
            if size >= minimum:
                return size
        raise ValueError(f"cannot determine patch size at 0x{address:x}")

def firmware_key(name: str) -> int:
    major, minor = (int(part) for part in name.split(".", 1))
    if major > 99 or minor > 99:
        raise ValueError(f"firmware version is not BCD-compatible: {name}")
    major_bcd = ((major // 10) << 4) | (major % 10)
    minor_bcd = ((minor // 10) << 4) | (minor % 10)
    return (major_bcd << 8) | minor_bcd


def generate(root: Path) -> str:
    firmware_dirs = sorted(
        (path for path in root.iterdir() if (path / "system/vsh/SceShellCore.elf").is_file()),
        key=lambda path: tuple(int(part) for part in path.name.split(".")),
    )
    records: list[str] = []
    for firmware_dir in firmware_dirs:
        shellcore = ShellCore(firmware_dir / "system/vsh/SceShellCore.elf")
        targets = shellcore.locate_targets()
        records.append(
            "  {\n"
            f"    .firmware = 0x{firmware_key(firmware_dir.name):04x}u,\n"
            f"    .name = \"{firmware_dir.name}\",\n"
            "    .targets = {"
            + ", ".join(
                "{.offset = "
                f"0x{targets[name]:x}u, .patch_size = "
                f"{shellcore.patch_size(targets[name])}u}}"
                for name in TARGET_NAMES
            )
            + "},\n  }"
        )
    return "/* Generated by tools/generate_shellcore_offsets.py. */\n" + ",\n".join(records) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    output = generate(args.root)
    if args.output:
        with args.output.open("w", encoding="utf-8", newline="\n") as file:
            file.write(output)
    else:
        print(output, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
