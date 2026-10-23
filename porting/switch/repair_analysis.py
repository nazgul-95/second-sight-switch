"""Correct version-locked false entries before the standard CFG validation.

The apparent calls at 29E705/29E72D/29E755 are embedded D3D data. Their
targets land inside real XMV MOV and DSOUND AND instructions.
Keep the raw analysis intact; demote only these false call-target records
in a separate inventory, then let the normal coalescence validator prove CFG.
"""
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

ROOT = Path(__file__).resolve().parent


def main():
    image = (ROOT / 'game_files/game_ntsc.xbe').read_bytes()
    assert hashlib.sha256(image).hexdigest() == '226ebc4834fbd2f8f7c7e657fb4b1c8b8a7f53105d9189e6615bca1f4983c83e'
    base = struct.unpack_from('<I', image, 0x104)[0]
    count, table = struct.unpack_from('<II', image, 0x11C)
    sections = [struct.unpack_from('<IIIII', image, table-base+i*0x38) for i in range(count)]

    def read(address, size):
        section = next(s for s in sections if s[1] <= address and address+size <= s[1]+s[4])
        offset = section[3]+address-section[1]
        return image[offset:offset+size]

    disasm = ROOT / 'analysis_boot/disasm'
    functions = json.loads((disasm / 'functions.json').read_text())
    xrefs = json.loads((disasm / 'xrefs.json').read_text())
    for start, end, target, site, owner, expected in [
        (0x2CE60D, 0x2CE83A, 0x2CE733, 0x29E705, 0x2CE732, bytes.fromhex('8b45fc')),
        (0x2CE60D, 0x2CE83A, 0x2CE75B, 0x29E72D, 0x2CE759, bytes.fromhex('89b980000000')),
        (0x2AE73F, 0x2AE7D3, 0x2AE783, 0x29E755, 0x2AE77F, bytes.fromhex('25ffff0000')),
    ]:
        instructions = list(Cs(CS_ARCH_X86, CS_MODE_32).disasm(read(start, end-start), start))
        assert instructions[-1].address + instructions[-1].size == end
        assert read(owner, len(expected)) == expected
        assert any(i.address == owner and owner < target < owner+i.size for i in instructions)
        calls = [int(x['from'], 16) for x in xrefs if int(x['to'], 16) == target and x['type'] == 'call']
        assert calls == [site], (target, calls)
        assert read(site, 1) == b'\xe8'
        assert site+5+struct.unpack('<i', read(site+1, 4))[0] == target
        entry = next(f for f in functions if int(f['start'], 16) == target)
        assert entry['detection_method'] == 'call_target'
        assert not entry['called_by'] and not entry['has_prologue']
        entry['detection_method'] = 'verified_linear_sweep_false_call'
        entry['evidence'] = f'False call in embedded D3D data at {site:#x}; target is inside instruction {owner:#x}'
    # These aliases have only local branch references, not callers. Retain
    # their records for the coalescence validator's exact interior census.
    for start, end, target in [(0x2CE60D, 0x2CE83A, 0x2CE7D0),
                               (0x2CFECA, 0x2D00A6, 0x2D008D),
                               (0x2F7248, 0x2F740D, 0x2F7374),
                               (0x2AE73F, 0x2AE7D3, 0x2AE790)]:
        refs = [x for x in xrefs if int(x['to'], 16) == target]
        assert all(x['type'] in ('jump', 'cond_jump')
                   and start <= int(x['from'], 16) < end for x in refs)
        decoded = list(Cs(CS_ARCH_X86, CS_MODE_32).disasm(read(start, end-start), start))
        assert any(i.address == target for i in decoded)
        assert not any(i.mnemonic == 'call' and i.op_str == hex(target) for i in decoded)
        entry = next(f for f in functions if int(f['start'], 16) == target)
        assert entry['detection_method'] == 'tail_jump_alias'
        assert not entry['called_by'] and not entry['has_prologue']
        entry['detection_method'] = 'verified_local_branch'
        entry['evidence'] = f'Only local branches within {start:#x}..{end:#x}; full CFG must be validated before merge'
    destination = ROOT / 'analysis_boot/functions_repaired.json'
    destination.write_text(json.dumps(functions, indent=2) + '\n')
    print('Verified three mid-instruction false entries and four local branch aliases; wrote', destination)


if __name__ == '__main__':
    main()

