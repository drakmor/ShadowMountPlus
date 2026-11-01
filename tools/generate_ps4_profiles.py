#!/usr/bin/env python3
"""Import audited NeoEnabler retail ABIs and reserve independent ShellCore padding.

Generation only: --neo points to NeoEnabler sources; no runtime dependency.
Validate every call/context against all available ELF variants before emission.
"""
import argparse
import ast
import hashlib
import json
from pathlib import Path
import re
import struct
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_OP_REG, X86_REG_EAX, X86_REG_RBP, X86_REG_RSP
from generate_shellcore_offsets import ShellCore


def rows(path):
    result = [ast.literal_eval(line.strip().rstrip(',').replace('{', '[').replace('}', ']'))
              for line in path.read_text().splitlines() if line.startswith(' {0x')]
    if not result or len({row[0] for row in result}) != len(result):
        raise ValueError(f'{path}: empty or duplicate firmware table')
    return result


def verify_hfr_attribute(image, attribute):
    """Verify ATTRIBUTE3's member from the SFO reader, not a fixed firmware ABI."""
    segment = image.executable
    text = image.data[segment.file_offset:segment.file_offset + segment.file_size]
    keys = set(image.string_addresses(b'ATTRIBUTE3\0', use_string_start=False))
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    stores = set()
    for match in re.finditer(rb'\x48\x8d\x35(....)', text, re.S):
        address = segment.virtual_address + match.start()
        if address + 7 + struct.unpack('<i', match[1])[0] not in keys:
            continue
        called = False
        for instruction in md.disasm(text[match.start():match.start() + 0x40], address):
            if instruction.mnemonic == 'call':
                called = True
            operands = instruction.operands
            if not called or instruction.mnemonic != 'mov' or len(operands) != 2:
                continue
            dst, src = operands
            if (dst.type == X86_OP_MEM and dst.size == 4
                and dst.mem.base not in (X86_REG_RBP, X86_REG_RSP) and not dst.mem.index
                and 0x100 <= dst.mem.disp < 0x400
                and src.type == X86_OP_REG and src.reg == X86_REG_EAX):
                stores.add(dst.mem.disp)
                break
    if stores != {attribute + 8}:
        raise ValueError(f'{image.path}: SFO ATTRIBUTE3 member mismatch: {stores}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--neo', type=Path, required=True)
    parser.add_argument('--shellcore', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    pros = {row[0]: row for row in rows(args.neo/'src/pro_firmware.h')}
    budgets = json.loads((args.neo/'docs/budget-profile-audit.json').read_text())
    audit = json.loads((args.neo/'docs/clock-profile-audit.json').read_text())
    pro_audit = json.loads((args.neo/'docs/pro-profile-audit.json').read_text())
    clocks = rows(args.neo/'src/clock_profiles.h')
    if {row[0] for row in clocks} != set(pros) or len(clocks) != len(audit):
        raise ValueError('Pro/clock firmware coverage differs')
    output, evidence = [], {}
    for row in clocks:
        fw, site, original, neo_slot, three, mode, size, before, after, entry, call = row
        _, pro_site, attribute, four, _, pro_before, pro_after = pros[fw]
        version = f'{fw >> 8:x}.{fw & 255:02x}'
        stem = 'SceShellCore'+version.replace('.', '')
        paths = sorted([*args.shellcore.glob(stem+'.elf'), *args.shellcore.glob(stem+'_*.elf')])
        if not paths:
            raise ValueError(f'{version}: no ELF')
        result = None
        evidence[version] = []
        for path in paths:
            image = ShellCore(path)
            verify_hfr_attribute(image, attribute)
            digest = hashlib.sha256(image.data).hexdigest()
            clock_proof = next((a for a in audit[version] if a['sha256'] == digest), None)
            pro_proof = next((a for a in pro_audit[version] if a['sha256'] == digest), None)
            if (not clock_proof or not pro_proof
                or clock_proof['fields'] != [site,original,neo_slot,three,mode,size]
                or pro_proof['fields'] != [pro_site,attribute,four]):
                raise ValueError(f'{path}: unaudited source or ABI')
            def read(address, length):
                offset = image.virtual_to_file(address)
                return image.data[offset:offset+length]
            if (read(site-16,16) != bytes(before) or read(site+5,16) != bytes(after)
                or read(original,32) != bytes(entry) or read(site,5) != bytes(call)
                or read(pro_site-16,16) != bytes(pro_before) or read(pro_site+5,3) != bytes(pro_after)):
                raise ValueError(f'{path}: ABI mismatch')
            pro_call = read(pro_site,5)
            if pro_call[0] != 0xe8:
                raise ValueError(f'{path}: not a CALL')
            pro_original = pro_site+5+struct.unpack_from('<i',pro_call,1)[0]
            pro_entry = read(pro_original,16)
            cave, _ = image.bridge_cave()
            pool = budgets[version]['launch_fields'][5:7]
            excluded = [(cave,cave+(0x2e8 if fw >= 0x1200 else 0x260)), tuple(pool), (neo_slot,neo_slot+256)]
            candidates = []
            for segment in image.loads:
                if segment.flags not in (1,4,5) or segment.memory_size != segment.file_size:
                    continue
                end = segment.virtual_address+segment.memory_size
                for slot in range((end+15)&~15, ((end+0x3fff)&~0x3fff)-159,16):
                    if any(start < slot+160 and slot < stop for start,stop in excluded):
                        continue
                    if any(s.virtual_address < slot+160 and slot < s.virtual_address+s.memory_size for s in image.loads):
                        continue
                    candidates.append((segment.flags == 4,slot))
            if not candidates:
                raise ValueError(f'{path}: no independent padding')
            slot = max(candidates)[1]
            current = [fw,site,original,neo_slot,slot,pro_site,pro_original,attribute,three,mode,size,four,
                       before,after,entry,call,pro_before,pro_after,list(pro_call),list(pro_entry)]
            if result is not None and current != result:
                raise ValueError(f'{version}: incompatible variants')
            result = current
            evidence[version].append({'sha256':digest,'slot':hex(slot), 'pro_original':hex(pro_original)})
        arrays = ', '.join('{'+', '.join(f'0x{x:02x}' for x in a)+'}' for a in result[12:])
        output.append('  {'+', '.join(hex(x) for x in result[:12])+', '+arrays+'},')
    root = Path(__file__).resolve().parents[1]
    files = [(root/'src/sm_ps4_profiles.inc', '/* GPL-3.0-only. Generated from audited NeoEnabler ABIs; see tools/generate_ps4_profiles.py. */\n'+'\n'.join(output)+'\n'),
             (root/'docs/ps4-profile-audit.json',json.dumps(evidence,indent=2)+'\n')]
    for path, content in files:
        if args.check:
            if path.read_text() != content:
                raise ValueError(f'{path}: output differs')
        else:
            path.write_bytes(content.encode())
    print('Validated',len(output),'retail PS4 profiles')

if __name__ == '__main__':
    main()
