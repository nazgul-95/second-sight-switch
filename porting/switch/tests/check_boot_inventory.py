"""Reject the known broken boot inventory before compiling an NRO."""
import argparse
import hashlib
import json
import pathlib
import re
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--generated", type=pathlib.Path, default=ROOT / "generated_boot")
    args = parser.parse_args()
    funcs = {}
    definitions = set()
    generated_names = set()
    for path in args.generated.glob("recomp_*.c"):
        text = path.read_text(encoding="utf-8-sig")
        for start, end in re.findall(r"Original: 0x([0-9A-F]+) - 0x([0-9A-F]+)", text):
            funcs[int(start, 16)] = int(end, 16)
        if path.name != "recomp_stubs_unresolved.c":
            generated_names.update(re.findall(r"void (sub_\w+)\(void\)\s*\{", text))
            definitions.update(int(a, 16) for a in re.findall(r"void sub_([0-9A-F]{8})\(void\)\s*\{", text))
    # Independently established by the saved Ghidra body inventories and XDK
    # cross-title comparisons. The old lift stopped at the first early return.
    required = {0x288B70: 0x288CDA, 0x28AC30: 0x28ACD4, 0x28E0E0: 0x28E5A5,
                0x2CE60D: 0x2CE83A, 0x2CFECA: 0x2D00A6, 0x2F7248: 0x2F740D,
                0x2AE73F: 0x2AE7D3}
    for start, end in required.items():
        assert funcs.get(start) == end, f"truncated boot function {start:#x}: {funcs.get(start)} != {end:#x}"
    dispatch = (args.generated / "recomp_dispatch.c").read_text()
    manual = (ROOT / "src/recomp_manual.c").read_text()
    overrides = {int(address, 16): name for address, name in re.findall(
        r"case 0x([0-9A-F]{8})u:\s*return (sub_\w+);", manual)}
    manual_seeds = 0
    seeds = json.loads((ROOT / "config/seed_functions.json").read_text())
    for row in seeds:
        address = int(row["start"], 16)
        if address in overrides:
            # Observed callbacks use a manual observer wrapper and an emitted
            # _recovered body. They are resolved before generated dispatch.
            name = overrides[address]
            wrapper = re.search(r"void " + re.escape(name)
                                + r"\(void\)\s*\{(.*?)\n\}", manual, re.S)
            assert wrapper, f"manual seed {address:#x} has no real wrapper body"
            recovered = re.findall(r"\b(sub_[0-9A-F]{8}_(?:recovered|observed))\(\)", wrapper[1])
            assert recovered, f"manual seed {address:#x} does not call an emitted callback body"
            assert all(name in generated_names for name in recovered), f"manual seed {address:#x} calls a missing callback body"
            manual_seeds += 1
            continue
        assert address in definitions, f"seed {address:#x} has no real emitted body"
        assert f"{{ 0x{address:08X}u," in dispatch, f"seed {address:#x} missing from dispatch"
    assert 0x27A490 not in definitions, "broken generated CRT copy body still compiled"
    assert "{ 0x0027A490u, (recomp_func_t)sub_0027A490 }" in dispatch
    assert "recover_target" not in (args.generated / "recomp_types.h").read_text(), "obsolete import recovery still active"
    xbe = (ROOT / "game_files/game_ntsc.xbe").read_bytes()
    assert hashlib.sha256(xbe).hexdigest() == "226ebc4834fbd2f8f7c7e657fb4b1c8b8a7f53105d9189e6615bca1f4983c83e"
    base = struct.unpack_from("<I", xbe, 0x104)[0]
    count, table = struct.unpack_from("<II", xbe, 0x11C)
    sections = [struct.unpack_from("<IIIII", xbe, table - base + i * 0x38) for i in range(count)]
    for address, dst, value in [(0x2AFCF7, 0x2CE33C, 0x2CDC64), (0x2AFD02, 0x2CE338, 0x2CDC68),
                                (0x2B379C, 0x2CE3E4, 0x2CDC6C), (0x2B37A7, 0x2CE3E0, 0x2CDC70)]:
        section = next(s for s in sections if s[1] <= address < s[1] + s[4])
        offset = section[3] + address - section[1]
        assert xbe[offset:offset + 11] == b"\xC7\x05" + struct.pack("<II", dst, value) + b"\xC3", f"constructor seed changed at {address:#x}"
    print(f"Boot inventory: 7 complete D3D/XMV/XPP/DSOUND bodies, {len(seeds)} callable seeds ({manual_seeds} recovered manual overrides), native CRT copy, 4 XBE-verified constructors")


if __name__ == "__main__":
    main()

