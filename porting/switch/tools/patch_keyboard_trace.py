"""Insert bounded, read-only observations at original text-entry boundaries."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
path = ROOT / "generated_boot/recomp_0005.c"
text = path.read_text(encoding="utf-8-sig")
include = '#include "../src/keyboard_trace.h"\n'
result = text
for address, obj in {
    "00053B72": "esi",  # fully initialized text-entry object
    "00053370": "MEM32(esp + 4)",
    "00052D70": "MEM32(esp + 4)",
    "00052C20": "MEM32(esp + 4)",
    "00053410": "MEM32(esp + 4)",
    "0004FED0": "MEM32(esp + 4)",
}.items():
    label = f"loc_{address}: ;\n"
    if result.count(label) != 1:
        raise ValueError("Missing or ambiguous original boundary " + address)
    caller = "MEM32(esp + 8)" if address == "00053B72" else "MEM32(esp)"
    call = f"    second_sight_keyboard_trace(0x{address}u, {obj}, {caller});\n"
    old_call = f"    second_sight_keyboard_trace(0x{address}u, {obj}, MEM32(esp));\n"
    if label + old_call in result:
        result = result.replace(label + old_call, label + call)
    if label + call not in result:
        result = result.replace(label, label + call)
if include not in result:
    result = include + result
if result != text:
    path.write_text(result, encoding="utf-8", newline="\n")
print("Keyboard observations installed (no guest writes)")

