"""Read-only source inventory of the Switch toolkit and current generated C."""
import ast
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parents[3]
fork = root / "external_versions/mksm-switch/xboxrecomp"
upstream = root / "external_versions/xboxrecomp"
generated = root / "porting/switch/generated_boot"
output = root / "porting/switch/references/xboxrecomp-usage-20261019.json"

def declarations(path):
    tree = ast.parse(path.read_text(encoding="utf-8-sig"))
    result = {}
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            result[node.name] = ast.dump(node, include_attributes=False)
    return result

comparisons = {}
for relative in ("tools/recomp/translator.py", "tools/recomp/lifter.py"):
    a, b = declarations(fork/relative), declarations(upstream/relative)
    comparisons[relative] = {
        "fork_definitions": len(a), "upstream_definitions": len(b),
        "upstream_only": sorted(b.keys()-a.keys()),
        "fork_only": sorted(a.keys()-b.keys()),
        "changed_shared": sorted(k for k in a.keys() & b.keys() if a[k] != b[k]),
    }

fallbacks, unimplemented, total_functions = [], {}, 0
function_start = re.compile(r"^void (sub_([0-9A-Fa-f]{8}))\(void\)\s*\n\{", re.M)
for path in sorted(generated.glob("recomp_*.c")):
    source = path.read_text(encoding="utf-8-sig")
    starts = list(function_start.finditer(source))
    total_functions += len(starts)
    for index, match in enumerate(starts):
        body = source[match.end():starts[index+1].start() if index+1 < len(starts) else len(source)]
        uses = [(n, line.strip()) for n, line in enumerate(body.splitlines(), 1)
                if "_flags" in line and not re.search(r"\bint\s+_flags\s*=", line)]
        writes = [line for _, line in uses if re.search(r"\b_flags\s*(?:[|&^+*/-]?=|\+\+|--)", line)]
        if uses and not writes:
            fallbacks.append({"function": match.group(1), "va": match.group(2),
                              "file": path.name, "consumer_count": len(uses),
                              "game_text": int(match.group(2),16) < 0x282200,
                              "examples": [line[:220] for _, line in uses[:2]]})
        for instruction in re.findall(r'RECOMP_UNIMPL\("([^"\n]+)"\s*[,)]', body):
            mnemonic = instruction.split()[0] if instruction else ""
            unimplemented[mnemonic] = unimplemented.get(mnemonic, 0)+1

result = {"upstream_pin": "eb0aa1d5fcb1ca34cdb6470fcfa471d87f431d73",
          "switch_donor_pin": "14741aa08b61018df373c4a9b61b150b3a0c9f3e",
          "generated_functions": total_functions,
          "fallback_candidate_functions": len(fallbacks),
          "fallback_candidate_game_text_functions": sum(x["game_text"] for x in fallbacks),
          "fallback_candidates": fallbacks,
          "unimplemented_instruction_counts": unimplemented,
          "source_comparison": comparisons,
          "limits": ["Static inventory, not an execution test or a proof every candidate is reachable.",
                     "Overlapping/misidentified function boundaries can produce false candidates.",
                     "Same method names are compared by AST; equality does not establish full ABI compatibility."]}
output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
print(json.dumps({k:v for k,v in result.items() if k not in ("fallback_candidates","source_comparison")}, ensure_ascii=False, indent=2))
for relative, comparison in comparisons.items():
    print(relative, json.dumps({"upstream_only":comparison["upstream_only"],
                               "changed_shared_count":len(comparison["changed_shared"])},ensure_ascii=False))
print(output)

