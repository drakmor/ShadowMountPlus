#!/usr/bin/env python3
"""Import audited retail DATA budget layouts from NeoEnabler (generation only)."""
import argparse
import ast
import json
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--neo', type=Path, required=True)
    parser.add_argument('--kernels', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    # Reuse the kernel ELF reader already used to derive the source audit,
    # including its independently mapped flat 9.05 archive.
    sys.path.insert(0, str(args.neo / 'tools'))
    from generate_budget_profiles import Image, MD
    from capstone.x86 import X86_OP_MEM, X86_OP_IMM
    source = args.neo / 'src/neo_budget_data.h'
    audit = json.loads((args.neo / 'docs/budget-profile-audit.json').read_text())
    rows = [ast.literal_eval(line.strip().rstrip(',').replace('{', '[').replace('}', ']'))
            for line in source.read_text().splitlines() if line.startswith(' {0x')]
    if len(rows) != len(audit) or len({row[0] for row in rows}) != len(rows):
        raise ValueError('Incomplete or duplicated firmware layouts')
    result = ['/* GPL-3.0-only. Generated from audited NeoEnabler DATA layouts. */']
    for row in rows:
        fw, *fields = row
        if len(fields) != 12:
            raise ValueError('Unexpected DATA profile field count')
        version = f'{fw >> 24:x}.{(fw >> 16) & 255:02x}'
        proof = audit[version]
        if fields != proof['data_fields']:
            raise ValueError(f'{version}: DATA layout differs from audit')
        extent = fields[10] + 2
        if (not 0 < extent <= 0x148 or any(
                offset < 0 or offset + size > extent
                for offset, size in zip(fields[6:], (8, 8, 8, 8, 2, 8)))):
            raise ValueError(f'{version}: container fields exceed the snapshot')
        image = args.kernels / (version + '.elf')
        kernel = Image(image)
        if kernel.digest != proof['kernel_sha256']:
            raise ValueError(f'{version}: kernel hash differs from audit')
        start = int(proof['constructor'], 16)
        instructions = list(MD.disasm(kernel.read(start, 0x1200), start))
        flag = next((i for i, ins in enumerate(instructions)
                     if ins.mnemonic == 'or' and
                     ins.operands[0].type == X86_OP_MEM and
                     ins.operands[0].mem.disp == fields[10] and
                     ins.operands[-1].type == X86_OP_IMM and ins.operands[-1].imm == 0x40), None)
        if flag is None:
            raise ValueError(f'{version}: constructor completion flag not found')
        instructions = instructions[:flag]
        additions = [i for i, ins in enumerate(instructions)
                     if ins.mnemonic == 'add' and ins.op_str.endswith(', 0x20000000')]
        bonus_proc_off = 0
        if additions:
            if len(additions) != 1 or fw < 0x09000000:
                raise ValueError(f'{version}: unrecognized extra 512 MiB branch')
            window = instructions[additions[0]-5:additions[0]+1]
            expected = [('test', f'byte ptr [rbx + {fields[10]+1:#x}], 0x20'),
                        ('je', None), ('test', 'byte ptr [r13 + 0x464], 2'),
                        ('mov', 'r15, r14'), ('jne', None), ('add', 'r12, 0x20000000')]
            if len(window) != len(expected) or any(
                    ins.mnemonic != mnemonic or (operands and ins.op_str != operands)
                    for ins, (mnemonic, operands) in zip(window, expected)):
                raise ValueError(f'{version}: changed 512 MiB predicates')
            bonus_proc_off = 0x464
        elif fw >= 0x09000000:
            raise ValueError(f'{version}: missing audited 512 MiB branch')
        result.append('  {' + ', '.join(hex(v) for v in [fw >> 16, *fields, bonus_proc_off]) + '},')
    output = '\n'.join(result) + '\n'
    target = Path(__file__).resolve().parents[1] / 'src/sm_ps4_memory_profiles.inc'
    if args.check:
        if target.read_text() != output:
            raise ValueError('Generated memory profiles differ')
    else:
        target.write_bytes(output.encode())
    print('Validated', len(rows), 'retail memory profiles against kernel hashes')


if __name__ == '__main__':
    main()
